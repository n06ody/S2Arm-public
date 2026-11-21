// 对照包诊断：程序键出库排除（强制现编）。两路配置，取第一路非空：
//   ① S2A_LIB_DENY=<逗号分隔 16hex 程序键>（本地调试，缺省 any 模式）；
//   ② APK 内 assets/game/lib_deny.txt——经 s2a_android_read_asset 直读进内存
//     （release 清环境但保留包内 asset；不落盘——直编缓存目录冷开局后才由首个缓存写建出，
//       拷盘版在首次取库时 fopen 写目标必 ENOENT、once 又不重试，对照静默失效过一次）。
// 文件格式（v2，首个非注释非空行可带一条 mode= 指令，缺省 any）：
//   mode=any          逐阶段键：请求任一阶段键在表 → 该阶段键被排（r3 旧语义）
//   mode=group        整请求组：组键（图形=本请求 vs 键，计算=cs 键）在表 → 整请求绕库
//   mode=group-allow  整请求组白名单：组键不在表 → 整请求绕库（二分收敛期写小集合用）
//   # 起注释行忽略；键 = 16 个十六进制字符，一行一个或逗号/空白分隔；重复键整表红；
//   超过 LIB_DENY_MAX 键或文件超 LIB_DENY_FILE_BYTES 一律整表红（0 项生效，不静默截断）。
// 表解析先整表校验再一次发布：合法前缀后接畸形行 = 整表红、0 项生效（部分生效没法当对照证据）。
// 发布后按键序排序 + 二分查找（数千键不拖取库）；三结局都留行：解析到键（WARN）/ 包内无此件（INFO）/ 读取或解析失败（红）。
// 取证打点（前台提交线程与后台预热都进 libpack_fetch，g_deny_mu 同步）：
//   - lib_deny_hit 枚举本请求全部匹配阶段、返回匹配位图（bit st = 该阶段键被排；组模式下
//     组键命中/未列入 → 整请求位图全 1）；fetch 早退时位图 0。
//   - 前台：位图由调用方带到本请求的同步编译入口，真进编译才合记一行「本请求已绕库 → 进入现编」
//     （全键+阶段+种类+本请求身份+组键；每键有界一次，表 g_deny_pair）——绕过与现编天然同键同
//     身份成对，不依赖跨请求日志排序；没绕过/没真编译的请求零取证行。
//   - 后台/预热（bg=1）：每个被排键打一行「取库绕过（后台/预热）」，表 g_deny_by 单记、不占前台成对名额。
//   - 库命中首中（deny 发布才启用，与 libpack_note_hit() 同条件=构建成功未坏）：每个命中键打
//     一行「库命中 程序键 <全键>（<阶段>，<前台|预热>，组 <组键>）」，去重键含前台/预热
//     （g_hit_fg / g_hit_bg 两表），登记满留溢出正证行。二分第一轮据此取实测命中组集合。
// 另键不受影响、照走库命中。不是产品语义。
extern long long s2a_android_read_asset(const char *name, void *buf, long long cap);
#ifndef LIB_DENY_MAX
#define LIB_DENY_MAX 16384            // 键表容量：须 ≥ 全库组键数（现库 9060 阶段键 / 组键数百，留余量）
#endif
#ifndef LIB_DENY_FILE_BYTES
#define LIB_DENY_FILE_BYTES (512 * 1024)   // deny 文件读取上限（9,060 行约 154 KB，留余量；满读 = 文件超限报红）
#endif
#define LIB_DENY_HIT_MAX LIB_DENY_MAX      // 首中登记表容量（每侧独立）
enum { LIB_DENY_ANY = 0, LIB_DENY_GROUP = 1, LIB_DENY_ALLOW = 2 };
static uint8_t g_deny[LIB_DENY_MAX][8]; static unsigned g_deny_n; static int g_deny_mode = LIB_DENY_ANY;
static pthread_once_t g_deny_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_deny_mu = PTHREAD_MUTEX_INITIALIZER;
static int lib_deny_key_cmp(const void *a, const void *b) { return memcmp(a, b, 8); }
static int lib_deny_tab_has(const uint8_t (*tab)[8], unsigned tn, const uint8_t k[8]) {
    return tab && tn && bsearch(k, tab, tn, 8, lib_deny_key_cmp) != NULL;
}
// 有序插入（去重登记用）：1=新插入、0=已在、-1=表满
static int lib_deny_tab_ins(uint8_t (*tab)[8], unsigned *tn, unsigned cap, const uint8_t k[8]) {
    unsigned lo = 0, hi = *tn;
    while (lo < hi) { unsigned m = (lo + hi) / 2; int c = memcmp(tab[m], k, 8); if (c < 0) lo = m + 1; else if (c > 0) hi = m; else return 0; }
    if (*tn >= cap) return -1;
    memmove(tab + lo + 1, tab + lo, (*tn - lo) * 8); memcpy(tab[lo], k, 8); (*tn)++;
    return 1;
}
static void lib_deny_key_str(char out[17], const uint8_t k[8]) {
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", k[i]);
}
static void lib_deny_parse(const char *s, const char *from) {
    uint8_t (*tmp)[8] = malloc((size_t)LIB_DENY_MAX * 8);
    unsigned n = 0; int bad = 0, mode = LIB_DENY_ANY, saw_mode = 0;
    char b[1400];
    if (!tmp) { nvn_plat_log(NVN_PLAT_LOG_RED, "shader", "出库对照：deny 表分配失败，不启用键排除"); return; }
    while (s && *s) {
        while (*s == ',' || *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        if (*s == '#') { while (*s && *s != '\n') s++; continue; }   // 注释行
        if (!strncmp(s, "mode=", 5) && !n && !saw_mode) {           // 指令行只在首键前一条
            s += 5;
            if (!strncmp(s, "group-allow", 11)) { mode = LIB_DENY_ALLOW; s += 11; }
            else if (!strncmp(s, "group", 5)) { mode = LIB_DENY_GROUP; s += 5; }
            else if (!strncmp(s, "any", 3)) { mode = LIB_DENY_ANY; s += 3; }
            else { bad = 1; break; }
            saw_mode = 1;
            if (*s && *s != ',' && *s != ' ' && *s != '\n' && *s != '\t' && *s != '\r' && *s != '#') { bad = 1; break; }
            continue;
        }
        if (n >= LIB_DENY_MAX) {
            snprintf(b, sizeof b, "出库对照：%s 超过 %d 键上限，整表不发布（0 项生效）", from, LIB_DENY_MAX);
            nvn_plat_log(NVN_PLAT_LOG_RED, "shader", b); free(tmp); return;
        }
        for (int i = 0; i < 16; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { bad = 1; break; }
        }
        if (bad) break;
        for (int i = 0; i < 8; i++) { unsigned x = 0; sscanf(s + i * 2, "%2x", &x); tmp[n][i] = (uint8_t)x; }
        n++; s += 16;
        if (*s && *s != ',' && *s != ' ' && *s != '\n' && *s != '\t' && *s != '\r' && *s != '#') { bad = 1; break; }
    }
    if (!bad && n > 1) {   // 排序 + 重复键整表红（部分生效不算对照证据）
        qsort(tmp, n, 8, lib_deny_key_cmp);
        for (unsigned i = 1; i < n; i++) if (!memcmp(tmp[i], tmp[i - 1], 8)) { bad = 1; break; }
    }
    if (bad) {
        snprintf(b, sizeof b, "出库对照：%s 第 %u 个键起格式错误，整表不发布（0 项生效）", from, n + 1);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", b);
        free(tmp);
        return;
    }
    memcpy(g_deny, tmp, n * 8); g_deny_n = n; g_deny_mode = mode;
    free(tmp);
    static const char *const mn[3] = {"any", "group", "group-allow"};
    if (n) {
        snprintf(b, sizeof b, "出库对照：%u 个程序键被 %s 排除出取库（模式 %s），强制现编", n, from, mn[mode]);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", b);
    } else {
        snprintf(b, sizeof b, "出库对照：%s 里没有可解析的程序键，不启用键排除", from);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", b);
    }
}
static char g_deny_buf[LIB_DENY_FILE_BYTES + 1];
static void lib_deny_init(void) {
    const char *s = getenv("S2A_LIB_DENY");
    if (s && *s) { lib_deny_parse(s, "S2A_LIB_DENY"); return; }
    long long n = s2a_android_read_asset("game/lib_deny.txt", g_deny_buf, LIB_DENY_FILE_BYTES);
    if (n == -2) { nvn_plat_log(NVN_PLAT_LOG_INFO, "shader", "出库对照：包内无 lib_deny.txt，不启用键排除"); return; }
    if (n == LIB_DENY_FILE_BYTES) {   // 满读 = 文件 ≥ 上限（正好等于上限的文件不存在——合法文件到 ~160 KB）
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", "出库对照：lib_deny.txt 达到读取上限（可能截断），不启用键排除");
        return;
    }
    if (n <= 0) {
        char m[96]; snprintf(m, sizeof m, "出库对照：lib_deny.txt 读取失败（%lld），不启用键排除", n);
        nvn_plat_log(NVN_PLAT_LOG_RED, "shader", m);
        return;
    }
    g_deny_buf[n] = 0;
    lib_deny_parse(g_deny_buf, "lib_deny.txt");
}
static uint8_t g_deny_by[LIB_DENY_MAX][8]; static unsigned g_deny_by_n;    // 后台单记「取库绕过」已打键（每键一次）
static uint8_t g_deny_pair[LIB_DENY_MAX][8]; static unsigned g_deny_pair_n; // 前台「绕库+现编」成对行已打键（每键一次）
// 枚举本请求全部匹配阶段，返回匹配位图（bit st = 该阶段键被排；ckey 恒用 bit0）。
// any：逐阶段键查表；group/group-allow：组键（图形=gkeys[0] 即 vs、计算=ckey）查表，
//   命中（group）/未列入（allow）→ 整请求位图全 1。bg=1：每个被排键各打一行单记（每键一次）。
static unsigned lib_deny_hit(const uint8_t *identity, const uint8_t (*gkeys)[8],
                             uint32_t stages, const uint8_t *ckey, int bg) {
    pthread_once(&g_deny_once, lib_deny_init);
    if (!g_deny_n) return 0;
    static const char *const stn[3] = {"vs", "fs", "gs"};
    int n = ckey ? 1 : (stages == 0x7u ? 3 : 2);
    unsigned mask = 0;
    if (g_deny_mode == LIB_DENY_ANY) {
        for (int st = 0; st < n; st++) {
            const uint8_t *k = ckey ? ckey : gkeys[st];
            if (lib_deny_tab_has(g_deny, g_deny_n, k)) mask |= 1u << st;
        }
    } else {
        const uint8_t *grp = ckey ? ckey : gkeys[0];
        int in = lib_deny_tab_has(g_deny, g_deny_n, grp);
        if (g_deny_mode == LIB_DENY_GROUP ? in : !in) mask = (1u << n) - 1;
    }
    if (!bg || !mask) return mask;
    for (int st = 0; st < n; st++) if (mask & (1u << st)) {
        const uint8_t *k = ckey ? ckey : gkeys[st];
        pthread_mutex_lock(&g_deny_mu);
        if (lib_deny_tab_ins(g_deny_by, &g_deny_by_n, LIB_DENY_MAX, k) > 0) {
            char ks[17], gs[17], m[224]; lib_deny_key_str(ks, k); lib_deny_key_str(gs, ckey ? ckey : gkeys[0]);
            snprintf(m, sizeof m, "出库对照：程序键 %s（%s）取库绕过（后台/预热，组 %s，身份 %02x%02x%02x%02x…）",
                     ks, ckey ? "cs" : stn[st], gs,
                     identity ? identity[0] : 0, identity ? identity[1] : 0,
                     identity ? identity[2] : 0, identity ? identity[3] : 0);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", m);
        }
        pthread_mutex_unlock(&g_deny_mu);
    }
    return mask;
}
// 真正进入同步编译处调：deny_mask 是本请求 libpack_fetch 的匹配位图（早退/没绕过 = 0 → 零取证行），
// 每个已绕库键合记一行「已绕库 → 进入现编」（每键有界一次、带本请求身份+组键；后台不走这里）。
static void lib_deny_note_compile(const uint8_t *identity, unsigned deny_mask,
                                  const uint8_t (*keys)[8], int nst, const char *kind) {
    if (!deny_mask) return;
    static const char *const stn[3] = {"vs", "fs", "gs"};
    for (int st = 0; st < nst && st < 3; st++) {
        if (!(deny_mask & (1u << st))) continue;
        const uint8_t *k = keys[st];
        pthread_mutex_lock(&g_deny_mu);
        if (lib_deny_tab_ins(g_deny_pair, &g_deny_pair_n, LIB_DENY_MAX, k) > 0) {
            char ks[17], gs[17], m[256]; lib_deny_key_str(ks, k); lib_deny_key_str(gs, keys[0]);
            snprintf(m, sizeof m, "出库对照：程序键 %s（%s，%s）本请求已绕库 → 进入现编（组 %s，身份 %02x%02x%02x%02x…）",
                     ks, kind, nst == 1 ? "cs" : stn[st], gs,
                     identity ? identity[0] : 0, identity ? identity[1] : 0,
                     identity ? identity[2] : 0, identity ? identity[3] : 0);
            nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", m);
        }
        pthread_mutex_unlock(&g_deny_mu);
    }
}
// 库命中首中取证（deny 表发布才启用；与 libpack_note_hit() 同条件 = 构建成功未坏，不数取到但绑定失败的）。
// 去重键含前台/预热（两表分开），每键一行「全键+阶段+组键+前台|预热」；登记满留溢出正证（首条与每 256 条）。
static uint8_t g_hit_fg[LIB_DENY_HIT_MAX][8], g_hit_bg[LIB_DENY_HIT_MAX][8];
static unsigned g_hit_fg_n, g_hit_bg_n, g_hit_ovf[2];
static void lib_deny_note_hit(const uint8_t k[8], const char *st, const uint8_t grp[8], int bg) {
    if (!g_deny_n || !k) return;
    uint8_t (*tab)[8] = bg ? g_hit_bg : g_hit_fg;
    unsigned *tn = bg ? &g_hit_bg_n : &g_hit_fg_n;
    pthread_mutex_lock(&g_deny_mu);
    int r = lib_deny_tab_ins(tab, tn, LIB_DENY_HIT_MAX, k);
    if (r > 0) {
        char ks[17], gs[17], m[224]; lib_deny_key_str(ks, k); lib_deny_key_str(gs, grp);
        snprintf(m, sizeof m, "出库对照：库命中 程序键 %s（%s，%s，组 %s）", ks, st, bg ? "预热" : "前台", gs);
        nvn_plat_log(NVN_PLAT_LOG_WARN, "shader", m);
    } else if (r < 0) {
        unsigned ov = ++g_hit_ovf[bg ? 1 : 0];
        if (ov == 1 || !(ov % 256)) {
            char m[192];
            snprintf(m, sizeof m, "出库对照：库命中登记溢出（%s 已满 %d 条，第 %u 条溢出）", bg ? "预热" : "前台", LIB_DENY_HIT_MAX, ov);
            nvn_plat_log(NVN_PLAT_LOG_RED, "shader", m);
        }
    }
    pthread_mutex_unlock(&g_deny_mu);
}
static void lib_deny_note_hit_gfx(const uint8_t (*keys)[8], int nst, int bg) {
    static const char *const stn[3] = {"vs", "fs", "gs"};
    for (int st = 0; st < nst && st < 3; st++) lib_deny_note_hit(keys[st], stn[st], keys[0], bg);
}
