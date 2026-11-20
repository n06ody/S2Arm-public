// 资源包收推送（测试层契约 tests/真机契约.md 第 1、2 节 S2A_PACK_APPLY_ONLY）：App 起来只做——
//   解开 Documents/资源包/.incoming/*.tar（按名字序，解完即删）→ 写 资源包/.applied = manifest.json 的 index_sha256 → 往 Documents/unpack.log 追加一行 → 退出。
//   失败行含「返回 -<码>」（执行器按这个判红）。每解完一个 tar 记一行「进度：…」；全部解完后删掉资源包下不在 index.tsv 里的旧文件（旧版分卷等）；
//   再逐条核 index.tsv 的文件在、大小对（-8），都对才写 .applied / index.applied.tsv——「已应用」= 机上真有。
//   iOS 上在后台线程跑（host_main.c），UIKit 照常起来——近 10 GB 在启动路径上同步跑会被启动看门狗杀（测试设备1 10-19 首局实测）。tar 是执行器用 `tar --format pax` 打的：只认普通文件、目录、pax 扩展头（path / size），
//   路径相对资源包根；绝对路径、含 .. 的路径、其他条目类型一律判失败（不猜）。
// zstd 只解压：官方单文件解码器（v1.5.7 build/single_file_libs 生成，BSD，许可 runtime/third_party/zstd/LICENSE，登记 THIRD_PARTY_LICENSES.md）；
//   只给压缩传输的 game.img 块用（img_unzst）。整份编进本文件：各平台（Mac 宿主、iOS、安卓、单测）都只链 pack_apply.c，不加构建清单
#include "../third_party/zstd/zstddeclib.c"
#include "native.h"
#include "s2a_docs.h"   // iOS 进程级容器 Documents（s2a_container_docs）
#include "s2a_launch_ctx.h"   // 资源包闸门按启动上下文定文档根（多款容器 = 本款 Documents/games/<游戏号>）
#include "../include/s2a_ui_text.h"   // 提示页 / 说明页文字：中文原宏 + _EN，S2A_UI 按界面语言取（#316）
#include "../include/s2a_selfcheck_page.h"   // 说明页标题「游戏资源不完整」与自检提示页同一句
__attribute__((weak)) int s2a_ui_zh(void) { return 1; }   // 界面语言（s2a_ui_text.h）：iOS ios/app.m、Mac macos/front_mac.m 强定义；别的平台 / 只链本文件的单测按中文
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int img_whole_or_absent(const char *root);   // 本体 game.img 在就核长度（定义在游戏库快判旁）
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// 写失败现场（10-20 测试设备2 巫师3 收推送连续「返回 -4」，看不出是哪个条目）：失败的那一步、路径与 errno，解包失败时写进 unpack.log
static char g_untar_err[2400];
static int g_untar_trunc;   // 本次解包碰到截短（读到的少于条目 / 包尾结束块）
static int g_fail_errno;   // 最近一次失败的 errno（只用来判「空间满」给码，s2a_install_code）
static void untar_err(const char *step, const char *path, int e) { g_fail_errno = e; snprintf(g_untar_err, sizeof g_untar_err, "%s %s：errno %d %s", step, path, e, strerror(e)); }
// App 包内的资源包（11-16 Apple TV 安装包导出，s2a_docs.h s2a_pack_root_in_bundle）只读：下面四个写资源包根的入口（s2a_pack_apply、s2a_pack_migrate_one、
//   img_place / img_update、partial_ask）先判，在包内就打一行【红】说明写不进只读区、跳过这次写，不去碰也不崩。正常路径走不到这些入口
//   （包内带 .applied = 清单 sha，闸门 / 游戏库判已就绪；写操作审计见合入说明）；没上电视验过，将来电视上跑，这一行就是定位依据
static int pack_in_bundle(const char *root, const char *what) {
#if defined(__APPLE__) && TARGET_OS_TV
    const char *b = s2a_launch_ctx_bundle_dir ? s2a_launch_ctx_bundle_dir() : NULL; size_t n = b ? strlen(b) : 0;
    if (n && root && !strncmp(root, b, n) && root[n] == '/') { LOG("【红】%s：资源包在 App 包内（只读区，Apple TV 安装包），写不进，这次跳过：%s", what, root); return 1; }
#endif
    (void)root; (void)what; return 0;
}
#include <ftw.h>
static int mkdirs(char *p) {   // 建 p 的各级父目录（p 是文件路径）；已存在但不是目录（同名文件挡路）也算失败
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; int r = mkdir(p, 0755), e = errno; struct stat st;
        if (r && e == EEXIST && (stat(p, &st) || !S_ISDIR(st.st_mode))) { r = -1; e = ENOTDIR; }
        if (r && e != EEXIST) { untar_err("建目录", p, e); *s = '/'; return -1; } *s = '/'; }
    return 0;
}
// 资源包根 root 下解文件到 p 之前清掉旧布局挡路的东西（「0.3.10 → 0.4.3 重装装不上」：同一位置旧版是文件、新版要目录，或反过来，
//   每次重解都在同一步 -4，点重试没用）：p 的上级里在 root 之下、同名却是文件 / 链接的 → 删掉；p 自己是目录（新版要放文件）→ 整棵删掉。
//   只动 root 之下、只在解包时动：这些都是资源包的旧内容，新清单不要它们。删不掉照旧走后面的报错（errno 原文进 host.log）
static int rm_tree_one(const char *f, const struct stat *st, int flag, struct FTW *w) { (void)st; (void)w; return flag == FTW_DP ? rmdir(f) : unlink(f); }
static void clear_stale(const char *root, char *p) {
    size_t rl = strlen(root); if (strncmp(p, root, rl) || p[rl] != '/') return;
    struct stat st;
    for (char *s = p + rl + 1; *s; s++) if (*s == '/') { *s = 0;
        if (!lstat(p, &st) && !S_ISDIR(st.st_mode)) { int r = unlink(p); LOG("资源包：旧布局的文件 %s 挡着新目录，%s", p + rl + 1, r ? strerror(errno) : "已删"); }
        *s = '/'; }
    if (!lstat(p, &st) && S_ISDIR(st.st_mode)) { int r = nftw(p, rm_tree_one, 16, FTW_DEPTH | FTW_PHYS); LOG("资源包：旧布局的目录 %s 占着新文件的位置，%s", p + rl + 1, r ? strerror(errno) : "已整棵删掉"); }
}
static uint64_t oct(const char *s, int n) { uint64_t v = 0; for (int i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (uint64_t)(s[i] - '0'); return v; }
static int safe(const char *p) { return *p && *p != '/' && !strstr(p, "../") && strcmp(p, "..") && !(strlen(p) >= 3 && !strcmp(p + strlen(p) - 3, "/..")); }
// IMG 目录表的路径区自检（D-4VAX：玩家机上本体目录表里出现「823/ac_2_b1_nxsystemdata/0100000000000823/」——相邻两条路径首尾拼成的一截，路径区偏移错位，
//   原先照解、每次同一处 -4）：路径非空、中间没有 NUL、不以「/」结尾、没有「//」、安全（不越出资源包根）；整表没有重复路径。任一不对 = 本体目录表被改坏，
//   不硬解（后面的条目可能同样错位、解出来的内容是错的），按「IMG 坏」报，让玩家删掉重新导入 / 重推
static int toc_path_ok(const uint8_t *p, uint32_t pl) {
    if (!pl || memchr(p, 0, pl) || p[pl - 1] == '/') return 0;
    char tmp[4200]; if (pl >= sizeof tmp) return 0; memcpy(tmp, p, pl); tmp[pl] = 0;
    return !strstr(tmp, "//") && safe(tmp);
}
static int rel_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int toc_has_dup(char **rels, size_t n) {   // 有重复路径 → 1
    if (n < 2) return 0; char **v = malloc(n * sizeof *v); if (!v) return 0; memcpy(v, rels, n * sizeof *v); qsort(v, n, sizeof *v, rel_cmp);
    int dup = 0; for (size_t i = 1; i < n && !dup; i++) dup = !strcmp(v[i - 1], v[i]); free(v); return dup;
}

// 解一个 tar 到 root；返回 0 成功，负数 = 失败码（-1 读不了 -2 头坏 -3 路径不安全 -4 写失败；认不出的条目类型跳过、不算失败）；*nf / *nb 累计文件数与字节
int s2a_untar(const char *tar, const char *root, unsigned *nf, uint64_t *nb) {
    g_untar_trunc = 0; g_untar_err[0] = 0;   // 每次从空现场开始：不写现场的失败不能贴上一次的（iOS 装机剃刀 #10）
    int fd = open(tar, O_RDONLY); if (fd < 0) { untar_err("打开包", tar, errno); return -1; }
    uint8_t h[512], buf[1 << 16]; char pax_path[4096] = ""; uint64_t pax_size = (uint64_t)-1; int rc = 0;
    for (;;) {
        // 没读到归档尾（全零块）就到文件末尾 / 只读到半个头 = 包被截短（执行器用 tar 打的包必有归档尾；截在条目边界时旧代码当成解完、把缺文件的状态记成已应用）
        ssize_t r = read(fd, h, 512); if (r >= 0 && r != 512) { untar_err("包被截短（没有归档尾）", tar, EIO); g_untar_trunc = 1; rc = -4; break; } if (r < 0) { untar_err("读包头", tar, errno); rc = -2; break; }
        int zero = 1; for (int i = 0; i < 512 && zero; i++) zero = !h[i]; if (zero) break;   // 归档尾（两块全零）
        unsigned sum = 0; for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (sum != oct((char *)h + 148, 8)) { untar_err("条目头校验和不对（包没传对）", tar, EINVAL); rc = -2; break; }
        char type = (char)h[156]; uint64_t size = oct((char *)h + 124, 12);
        char name[4096];
        if (h[345]) snprintf(name, sizeof name, "%.155s/%.100s", (char *)h + 345, (char *)h); else snprintf(name, sizeof name, "%.100s", (char *)h);
        if (pax_size != (uint64_t)-1) size = pax_size;
        if (pax_path[0]) strlcpy(name, pax_path, sizeof name);
        uint64_t pad = (512 - size % 512) % 512;
        if (type == 'x' || type == 'g') {   // pax 扩展头：「长度 键=值\n」记录；只取 path / size，作用于下一个条目（g 全局头忽略）
            if (size > sizeof buf - 1 || read(fd, buf, size) != (ssize_t)size || lseek(fd, (off_t)pad, SEEK_CUR) < 0) { untar_err("读 pax 扩展头", tar, EINVAL); rc = -2; break; }
            buf[size] = 0;   // strtoul 不越过本头数据
            pax_path[0] = 0; pax_size = (uint64_t)-1;
            if (type == 'g') continue;
            for (uint64_t o = 0; o < size;) {
                char *rec = (char *)buf + o; unsigned long len = strtoul(rec, NULL, 10); if (!len || o + len > size) { untar_err("pax 扩展头记录长度不对", tar, EINVAL); rc = -2; break; }
                // 记录格式「长度 键=值\n」：空格与换行都得在本条记录内（11-01 审计 P2-17：缺空格时 strchr 得 NULL、+1 后解引用崩；按整条长度找「=」会越过记录尾），不符判头坏
                char *sp = memchr(rec, ' ', len), *end = rec + len - 1;
                if (!sp || *end != '\n') { untar_err("pax 扩展头记录格式异常（缺空格或换行）", tar, EINVAL); rc = -2; break; }
                char *kv = sp + 1, *eq = memchr(kv, '=', (size_t)(end - kv));
                if (!eq) { untar_err("pax 扩展头记录格式异常（缺等号）", tar, EINVAL); rc = -2; break; }
                if (eq && eq - kv == 4 && !memcmp(kv, "path", 4) && (size_t)(end - eq - 1) < sizeof pax_path) { memcpy(pax_path, eq + 1, (size_t)(end - eq - 1)); pax_path[end - eq - 1] = 0; }
                if (eq && eq - kv == 4 && !memcmp(kv, "size", 4)) pax_size = strtoull(eq + 1, NULL, 10);
                o += len;
            }
            if (rc) break;
            continue;
        }
        pax_path[0] = 0; pax_size = (uint64_t)-1;
        if (!strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name + 2) + 1);
        if (type == '5') { if (!*name) continue; if (!safe(name)) { untar_err("路径不安全", name, EINVAL); rc = -3; break; } char p[8192]; snprintf(p, sizeof p, "%s/%s/x", root, name); if (mkdirs(p)) { rc = -4; break; } continue; }
        // 普通文件 / 目录 / pax 头以外的条目（链接、设备等）：执行器打的包里没有，v0.3.1 起认不出的不挡整次解包——跳过它的数据、记一笔，
        //   真缺了什么由后面 index.tsv 核对按缺文件 / 部分可玩处理（原先返回 -5，整个资源包「安装没完成」）
        if (type != '0' && type != 0) {
            LOG("【黄】解包：%s 条目类型 %c 认不出，跳过（%llu 字节）", name, type, (unsigned long long)size);
            if (lseek(fd, (off_t)(size + pad), SEEK_CUR) < 0) { untar_err("跳过认不出的条目", name, errno); rc = -2; break; }
            continue;
        }
        if (!safe(name)) { LOG("【红】解包：路径不安全 %s", name); untar_err("路径不安全", name, EINVAL); rc = -3; break; }
        char p[8192], tmp[8300]; snprintf(p, sizeof p, "%s/%s", root, name); snprintf(tmp, sizeof tmp, "%s.part", p);
        clear_stale(root, p);
        if (mkdirs(p)) { rc = -4; break; }
        int of = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (of < 0) { untar_err("建临时文件", tmp, errno); rc = -4; break; }
        for (uint64_t left = size; left;) {
            size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
            ssize_t rr = read(fd, buf, k); if (rr != (ssize_t)k) { untar_err(rr < 0 ? "读包" : "包被截短（读到的少于条目大小）", name, rr < 0 ? errno : EIO); if (rr >= 0) g_untar_trunc = 1; rc = -4; break; }
            if (write(of, buf, k) != (ssize_t)k) { untar_err("写文件", tmp, errno); rc = -4; break; }
            left -= k;
        }
        if (close(of) && !rc) { untar_err("关文件", tmp, errno); rc = -4; }
        if (!rc && rename(tmp, p)) { untar_err("改名到", p, errno); rc = -4; }
        if (rc) { unlink(tmp); break; }
        if (pad && lseek(fd, (off_t)pad, SEEK_CUR) < 0) { untar_err("跳过条目填充", name, errno); rc = -2; break; }
        (*nf)++; *nb += size;
    }
    close(fd); return rc;
}

#include <stdarg.h>
#include <ftw.h>
static void log_line(const char *logp, const char *fmt, ...) {
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&now));
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    FILE *f = fopen(logp, "a"); if (f) { fprintf(f, "%s %s\n", ts, b); fclose(f); }
}
// 清旧文件：遍历资源包根，删掉不在 index.tsv 里的普通文件（index.tsv / manifest.json / 以 . 开头的执行器与 App 记账文件、.incoming 目录除外）
// ---- 单一 IMG v1（pack/img.py，v0.3.0）：资源包根的 game.img。RomFS 由服务层按偏移直接读；exefs / ctl / aoc / 系统数据这类小文件在这里一次性解到资源包根，
//   运行时照旧按路径读（不改各读取点）。头：0 "S2AIMG01" | 8 u32 格式号 | 112/120 目录表偏移 / 长 | 128 整个文件长；
//   目录表：u32 槽数 | u32 条目数 | 槽数 × {u64 路径哈希（0 = 空槽）, u64 偏移, u64 长度, u32 路径偏移, u32 路径长} | 路径区。romfs.list 是给服务层的、不解
typedef struct { char *rel; uint64_t off, len; } img_ent;
// 应用资源包的分段进度（游戏库「安装中」扇形蒙版用，s2a_pack_apply_progress 照旧只报待解包数）：
//   phase 1 = 解开推送包（done / total = 包数）；2 = 从 game.img 解小文件（字节）；3 = 核对机上文件（文件数）。主调方设、本文件只调，NULL = 不报
void (*s2a_pack_apply_phase)(int phase, uint64_t done, uint64_t total);
#define PHASE(ph, d, t) do { if (s2a_pack_apply_phase) s2a_pack_apply_phase((ph), (d), (t)); } while (0)
static int img_toc_file(const char *p, img_ent **out, size_t *n);
static int img_toc(const char *root, img_ent **out, size_t *n) {   // 0 = 读到（*n 条）；1 = 没有 game.img；负数 = IMG 坏 / 格式号不认
    char p[2200]; snprintf(p, sizeof p, "%s/game.img", root); return img_toc_file(p, out, n);
}
static int img_toc_file(const char *p, img_ent **out, size_t *n) {   // 同上，读任意路径的 IMG（导入时认领前先看它带不带着色器库）
    *out = NULL; *n = 0;
    int fd = open(p, O_RDONLY); if (fd < 0) return 1;
    uint8_t h[136]; struct stat st; int rc = -10;
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || memcmp(h, "S2AIMG01", 8) || fstat(fd, &st)) goto out;
    uint32_t fmt; memcpy(&fmt, h + 8, 4);
    if (fmt != 1) LOG("资源包：game.img 格式号 %u 这个 S2Arm 不认识（只认 1），按格式 1 照常读；读不出来再报（维护者 11-17：校验不过不整个拒绝）", fmt);
    uint64_t to, tl, total; memcpy(&to, h + 112, 8); memcpy(&tl, h + 120, 8); memcpy(&total, h + 128, 8);
    if (total != (uint64_t)st.st_size || tl < 8 || tl > (64u << 20) || to + tl > total) goto out;
    uint8_t *t = malloc(tl); if (!t || pread(fd, t, tl, (off_t)to) != (ssize_t)tl) { free(t); goto out; }
    uint32_t slots; memcpy(&slots, t, 4); size_t pbase = 8 + (size_t)slots * 32;
    if (pbase > tl) { free(t); goto out; }
    img_ent *e = calloc(slots ? slots : 1, sizeof *e); size_t k = 0;
    for (uint32_t i = 0; i < slots; i++) { const uint8_t *s = t + 8 + (size_t)i * 32; uint64_t hh; memcpy(&hh, s, 8); if (!hh) continue;
        uint32_t po, pl; memcpy(&e[k].off, s + 8, 8); memcpy(&e[k].len, s + 16, 8); memcpy(&po, s + 24, 4); memcpy(&pl, s + 28, 4);
        if (pbase + po + pl > tl || e[k].off + e[k].len > total || !toc_path_ok(t + pbase + po, pl)) {
            if (pbase + po + pl <= tl) untar_err("IMG 目录表路径不对（本体被改坏）", (const char *)t + pbase + po, EINVAL);
            for (size_t j = 0; j < k; j++) free(e[j].rel); free(e); free(t); goto out; }
        e[k].rel = strndup((const char *)t + pbase + po, pl);
        if (!strcmp(e[k].rel, "romfs.list")) { free(e[k].rel); continue; }
        k++; }
    free(t);
    { char **rels = malloc((k ? k : 1) * sizeof *rels); if (rels) { for (size_t j = 0; j < k; j++) rels[j] = e[j].rel;
        int dup = toc_has_dup(rels, k); free(rels);
        if (dup) { untar_err("IMG 目录表有重复路径（本体被改坏）", p, EINVAL); for (size_t j = 0; j < k; j++) free(e[j].rel); free(e); goto out; } } }
    *out = e; *n = k; rc = 0;
out:
    close(fd); return rc;
}
static void img_free(img_ent *e, size_t n) { for (size_t i = 0; i < n; i++) free(e[i].rel); free(e); }
// 把目录表里的文件解到资源包根：已在且长度对的跳过（重复调用是空操作）；先写 .img.tmp 再改名，断了不留半个文件。→ 0 / 负数（g_untar_err 记现场）
static int img_extract(const char *root, unsigned *nf, uint64_t *nb) {
    img_ent *e; size_t n; int rc = img_toc(root, &e, &n); if (rc) return rc > 0 ? 0 : rc;
    char ip[2200]; snprintf(ip, sizeof ip, "%s/game.img", root); int fd = open(ip, O_RDONLY); if (fd < 0) { img_free(e, n); return -10; }
    uint8_t *buf = malloc(1 << 20); rc = 0;
    uint64_t all = 0, got = 0; for (size_t i = 0; i < n; i++) all += e[i].len;
    PHASE(2, 0, all);
    for (size_t i = 0; i < n && !rc; i++) {
        if (!safe(e[i].rel)) { untar_err("IMG 路径不安全", e[i].rel, EINVAL); rc = -3; break; }
        char p[2400], tmp[2500]; struct stat st; snprintf(p, sizeof p, "%s/%s", root, e[i].rel);
        if (!stat(p, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == e[i].len) { got += e[i].len; PHASE(2, got, all); continue; }
        clear_stale(root, p);
        if (mkdirs(p)) { rc = -4; break; }
        snprintf(tmp, sizeof tmp, "%s.img.tmp", p); int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (o < 0) { untar_err("建", tmp, errno); rc = -4; break; }
        for (uint64_t done = 0; done < e[i].len && !rc;) { size_t want = e[i].len - done > (1u << 20) ? (1u << 20) : (size_t)(e[i].len - done);
            ssize_t r = pread(fd, buf, want, (off_t)(e[i].off + done));
            if (r <= 0 || write(o, buf, (size_t)r) != r) { untar_err("从 IMG 解", e[i].rel, errno); rc = -4; } else { done += (uint64_t)r; got += (uint64_t)r; PHASE(2, got, all); } }
        if (close(o) && !rc) { untar_err("写完关闭", tmp, errno); rc = -4; }
        if (!rc && rename(tmp, p)) { untar_err("改名", p, errno); rc = -4; }
        if (rc) unlink(tmp); else { (*nf)++; *nb += e[i].len; }
    }
    free(buf); close(fd); img_free(e, n); return rc;
}
// ---- 压缩传输的 game.img（v0.5.0，tools/push_pack.py send_zst）：电脑把 game.img 按 S2A_ZCHUNK 原始字节切块、每块压成一个 zstd 帧（带内容校验），
//   推成 .incoming/game.img.<资源包号前 16 位>.<块号 4 位>.zst（资源包号 = index.tsv 里 game.img 那行第三列）；块号 k 的原始字节在 game.img 的 k × S2A_ZCHUNK 处。机上仍存未压缩的 game.img，运行时零改动。
//   这里按块号顺序流式解（ZSTD_decompressStream，1 MB 读写缓冲，不先解出整块）、pwrite 到位、fsync 后删掉这一块：
//   机上峰值 ≤ 未压缩 game.img + 一块（已解的部分 + 剩下的压缩块），不落整份压缩件再解。
//   号不是 index.tsv 里 game.img 那行的（别的版本留下的）、空文件（推送端盖掉的）→ 删掉不解。
//   第 0 块截断 game.img 从头写；第 k 块要 game.img 已有 ≥ k × S2A_ZCHUNK（前面的块解过了、只在解完后删），不然就是缺块；
//   缺块 / 截短 / 帧校验不对 / 解出的字节数不是整块（最后一块 1..S2A_ZCHUNK）→ 按截短包处理：删掉这块与其后各块、推送记录 .pushed，
//   电脑端下次不信推送记录、按已应用索引重推（tools/push_pack.py plan）。块大小是两端的契约，改要两边一起改（tests/push_pack_test.py 核两边同值）
#ifndef S2A_ZCHUNK
#define S2A_ZCHUNK (1ULL << 30)
#endif
static int img_sha16(const char *root, char out[17]) {   // index.tsv 里 game.img 那行第三列（资源包号）的前 16 位；没有这行 → -1
    char ip[2100], line[4600]; snprintf(ip, sizeof ip, "%s/index.tsv", root); FILE *f = fopen(ip, "r"); if (!f) return -1;
    int rc = -1;
    while (rc && fgets(line, sizeof line, f)) {
        if (strncmp(line, "game.img\t", 9)) continue;
        char *s = strchr(line + 9, '\t'); if (!s || strlen(s + 1) < 16) break;
        memcpy(out, s + 1, 16); out[16] = 0; rc = 0; }
    fclose(f); return rc;
}
static int zchunk_name(const char *n, char sha[17], unsigned *k) {   // game.img.<16 位小写十六进制>.<4 位块号>.zst → 1
    if (strlen(n) != 34 || strncmp(n, "game.img.", 9) || n[25] != '.' || strcmp(n + 30, ".zst")) return 0;
    for (int i = 9; i < 25; i++) if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    for (int i = 26; i < 30; i++) if (n[i] < '0' || n[i] > '9') return 0;
    memcpy(sha, n + 9, 16); sha[16] = 0; *k = (unsigned)atoi(n + 26); return 1;
}
// 一块流式解进 ofd 的 off 处：0 成功（*got = 解出字节）；-4 截短 / 读写失败；-2 帧坏（校验不对、不是 zstd）
static int unzst_one(const char *src, int ofd, uint64_t off, uint64_t *got) {
    *got = 0; int fd = open(src, O_RDONLY); if (fd < 0) { untar_err("打开压缩块", src, errno); return -4; }
    ZSTD_DStream *z = ZSTD_createDStream(); size_t isz = ZSTD_DStreamInSize(), osz = 1 << 20;
    uint8_t *ib = malloc(isz), *ob = malloc(osz); int rc = 0; ssize_t r = 0; size_t ret = 1;
    if (!z || !ib || !ob) { untar_err("解压缓冲", src, ENOMEM); rc = -4; goto out; }
    ZSTD_initDStream(z);
    // 官方流式示例的口径：完整的帧在吐完全部输出之前不会吃掉最后一个输入字节，所以「这段输入吃完」即可换下一段；读到文件尾时帧还没收尾 = 截短
    while (!rc && (r = read(fd, ib, isz)) > 0) {
        ZSTD_inBuffer in = {ib, (size_t)r, 0};
        while (!rc && in.pos < in.size) {
            if (!ret) { untar_err("压缩块帧后还有多余字节", src, EINVAL); rc = -2; break; }   // 一块就一帧
            ZSTD_outBuffer out = {ob, osz, 0};
            ret = ZSTD_decompressStream(z, &out, &in);
            if (ZSTD_isError(ret)) { untar_err("压缩块帧坏（校验不对或不是 zstd）", src, EINVAL); rc = -2; break; }
            if (*got + out.pos > S2A_ZCHUNK) { untar_err("压缩块解出来超过一块", src, EINVAL); rc = -2; break; }
            for (size_t w = 0; w < out.pos;) { ssize_t k = pwrite(ofd, ob + w, out.pos - w, (off_t)(off + *got + w)); if (k <= 0) { untar_err("写 game.img", src, errno); rc = -4; break; } w += (size_t)k; }
            *got += out.pos;
        }
    }
    if (!rc && r < 0) { untar_err("读压缩块", src, errno); rc = -4; }
    if (!rc && ret) { untar_err("压缩块被截短（帧没收尾）", src, EIO); g_untar_trunc = 1; rc = -4; }
out:
    free(ib); free(ob); ZSTD_freeDStream(z); close(fd); return rc;
}
// 解 .incoming 里本份 game.img 的压缩块 → 0 / 负数（g_untar_err 记现场）；*n 解了几块、*nb 解出字节
static int img_unzst(const char *root, const char *inc, const char *logp, unsigned *n, uint64_t *nb) {
    *n = 0; char want[17]; if (img_sha16(root, want)) want[0] = 0;
    DIR *d = opendir(inc); if (!d) return 0;
    unsigned ks[4096]; int nk = 0, rc = 0; struct dirent *e;
    while ((e = readdir(d))) { char s[17]; unsigned k; char p[2400]; struct stat st;
        if (!zchunk_name(e->d_name, s, &k)) continue;
        snprintf(p, sizeof p, "%s/%s", inc, e->d_name);
        if (strcmp(s, want) || stat(p, &st) || !st.st_size) { int r = unlink(p); log_line(logp, "进度：删掉不属于这份 game.img 的压缩块 %s（%s）", e->d_name, r ? strerror(errno) : "已删"); continue; }
        if (nk < (int)(sizeof ks / sizeof *ks)) ks[nk++] = k; }
    closedir(d);
    if (!nk) return 0;
    for (int i = 1; i < nk; i++) for (int j = i; j > 0 && ks[j - 1] > ks[j]; j--) { unsigned t = ks[j]; ks[j] = ks[j - 1]; ks[j - 1] = t; }
    char ip[2200]; snprintf(ip, sizeof ip, "%s/game.img", root);
    PHASE(1, 0, (uint64_t)nk);
    for (int i = 0; i < nk && !rc; i++) {
        char p[2400]; snprintf(p, sizeof p, "%s/game.img.%s.%04u.zst", inc, want, ks[i]);
        uint64_t off = (uint64_t)ks[i] * S2A_ZCHUNK, got = 0, t1 = now_ns(); struct stat st;
        int ofd = open(ip, O_WRONLY | O_CREAT | (ks[i] == 0 ? O_TRUNC : 0), 0644);
        if (ofd < 0) { untar_err("打开 game.img 写", ip, errno); rc = -4; break; }
        if (ks[i] && (fstat(ofd, &st) || (uint64_t)st.st_size < off)) { untar_err("缺压缩块（前一块没解进 game.img）", p, EIO); g_untar_trunc = 1; rc = -4; }
        if (!rc) rc = unzst_one(p, ofd, off, &got);
        // 不是最后一块就得正好一整块；最后一块解完把 game.img 截到实长（旧的更长的内容不留）
        if (!rc && i + 1 < nk && got != S2A_ZCHUNK) { untar_err("压缩块解出来不足一块", p, EIO); g_untar_trunc = 1; rc = -4; }
        if (!rc && i + 1 == nk && (!got || ftruncate(ofd, (off_t)(off + got)))) { untar_err(got ? "截 game.img 到实长" : "最后一块是空的", ip, got ? errno : EIO); rc = -4; }
        if (!rc && fsync(ofd)) { untar_err("落盘 game.img", ip, errno); rc = -4; }
        close(ofd);
        if (rc) { g_untar_trunc = 1;   // 这块与其后各块、推送记录都删：电脑端下次按已应用索引重推（同截短 tar）
            for (int j = i; j < nk; j++) { char u[2400]; snprintf(u, sizeof u, "%s/game.img.%s.%04u.zst", inc, want, ks[j]); unlink(u); }
            char pp[2100]; snprintf(pp, sizeof pp, "%s/.pushed", root); unlink(pp); break; }
        unlink(p); (*n)++; *nb += got;
        PHASE(1, (uint64_t)(i + 1), (uint64_t)nk);
        log_line(logp, "进度：解开 game.img.%s.%04u.zst（%d/%d）%llu MB（%.1f 秒）", want, ks[i], i + 1, nk, (unsigned long long)(got >> 20), (now_ns() - t1) / 1e9);
    }
    return rc;
}
// .incoming 里有没有压缩块（导入状态判「待导入」用；不分是不是本份，解的时候再分）
static int has_zchunks(const char *inc) {
    DIR *d = opendir(inc); if (!d) return 0; struct dirent *e; int any = 0; char s[17]; unsigned k;
    while (!any && (e = readdir(d))) any = zchunk_name(e->d_name, s, &k);
    closedir(d); return any;
}
// 资源包根有 game.img、还没按这份清单应用过（.applied ≠ manifest 的 index_sha256）→ 要先解开（导入状态判「待导入」，游戏库 / 闸门照常触发 s2a_pack_apply）
static int manifest_sha(const char *path, char *out, size_t n);
//   清单读不出（推送开头置空了、还没写回）按「要解」：原先当「不用解」，闸门判就绪放行半截镜像（iOS 装机剃刀 #29）；在不在传由闸门另判（s2a_import_transferring）
static int img_needs_apply(const char *root) {
    char p[2200], want[200], got[200] = ""; struct stat st; snprintf(p, sizeof p, "%s/game.img", root); if (stat(p, &st)) return 0;
    snprintf(p, sizeof p, "%s/manifest.json", root); if (manifest_sha(p, want, sizeof want)) return 1;
    snprintf(p, sizeof p, "%s/.applied", root); FILE *f = fopen(p, "r"); if (f) { size_t k = fread(got, 1, sizeof got - 1, f); got[k] = 0; got[strcspn(got, "\r\n")] = 0; fclose(f); }
    return strcmp(got, want) != 0;
}

static char **g_keep; static size_t g_nkeep, g_rootlen; static unsigned *g_nd; static uint64_t *g_db;
static int keep_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int prune_one(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)f; if (flag != FTW_F) return 0;
    const char *rel = p + g_rootlen + 1;
    if (!strcmp(rel, "index.tsv") || !strcmp(rel, "index.applied.tsv") || !strcmp(rel, "manifest.json") || rel[0] == '.' || !strncmp(rel, ".incoming/", 10)) return 0;   // index.applied.tsv：核对不过时它得留着（执行器据它补推）
    if (bsearch(&rel, g_keep, g_nkeep, sizeof *g_keep, keep_cmp)) return 0;
    if (!unlink(p)) { (*g_nd)++; *g_db += (uint64_t)st->st_size; }
    return 0;
}
// 删空了的目录也清掉（10-25 测试设备2 马车8：旧直连库的 shaders/direct/b-<链>/ 文件删光、目录还在，开局「包里有 b-… 但没有 shader_map.tsv」报红误导排查）。
//   后序遍历（子目录先于父目录），只删空目录（rmdir 非空即失败、不管）；根、以 . 开头的目录（.incoming 等记账目录）及其下不动
static int prune_dir(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; if (flag != FTW_DP || f->level == 0) return 0;
    const char *rel = p + g_rootlen + 1;
    if (rel[0] == '.' || strstr(rel, "/.")) return 0;
    rmdir(p);
    return 0;
}
static int prune(const char *root, unsigned *nd, uint64_t *db) {
    char ip[2100]; snprintf(ip, sizeof ip, "%s/index.tsv", root);
    FILE *f = fopen(ip, "r"); if (!f) return -7;
    size_t cap = 1024; g_keep = malloc(cap * sizeof *g_keep); g_nkeep = 0; char line[4600];
    while (fgets(line, sizeof line, f)) { char *t = strchr(line, '\t'); if (!t) continue; *t = 0; if (g_nkeep == cap) g_keep = realloc(g_keep, (cap *= 2) * sizeof *g_keep); g_keep[g_nkeep++] = strdup(line); }
    fclose(f);
    { img_ent *e; size_t n; if (!img_toc(root, &e, &n)) { for (size_t i = 0; i < n; i++) { if (g_nkeep == cap) g_keep = realloc(g_keep, (cap *= 2) * sizeof *g_keep); g_keep[g_nkeep++] = strdup(e[i].rel); } img_free(e, n); } }   // 从 game.img 解出来的小文件不在 index.tsv 里，不能当旧文件删
    qsort(g_keep, g_nkeep, sizeof *g_keep, keep_cmp);
    g_rootlen = strlen(root); g_nd = nd; g_db = db;
    nftw(root, prune_one, 32, FTW_PHYS);
    nftw(root, prune_dir, 32, FTW_PHYS | FTW_DEPTH);
    for (size_t i = 0; i < g_nkeep; i++) free(g_keep[i]); free(g_keep);
    return 0;
}

// 核对机上实况（10-25 测试设备2 马车8：执行器推了 4 批、App 解开 0 个包，照样把新 index.tsv 记成 index.applied.tsv / .applied，
//   之后两局执行器信这份「已应用」只推 3 个文件，机上没有直连库、退回通用）：index.tsv 每一行的文件都在、是普通文件、大小对得上，才算完整。
//   返回缺 / 不符的个数，ex 记第一个（「路径（不在 / 大小不符 / 不是文件）」，进日志）；progress = 1 时报「校验文件」阶段进度（解包那步）。
//   解包核对与闸门 s2a_pack_check 共用这一份（iOS 装机剃刀 #14：原先两份，闸门那份不查是不是普通文件，同名目录占位时两边结论相反）。
//   逐个 stat，九款最大索引约 39 万行、本机约 1 秒
// 起局离不了的核心文件（v0.3.1 缺件照玩）：程序（exefs/）、标题信息、单一 IMG / 旧 romfs 镜像。缺它们照旧不放行（物理上起不来）；
//   缺别的（旧形态的 romfs 散文件、着色器库、系统数据等）让玩家确认后照玩，真读到时游戏 Break → s2a_pack_help_on_break 说人话
int s2a_pack_core_path(const char *rel) {
    return !strncmp(rel, "exefs/", 6) || !strcmp(rel, "ctl/control.nacp") || !strcmp(rel, "game.img") || !strcmp(rel, "romfs.img");
}
static unsigned index_check_core(const char *root, char *ex, size_t exn, int progress, unsigned *core);
static unsigned index_check(const char *root, char *ex, size_t exn, int progress) { return index_check_core(root, ex, exn, progress, NULL); }
static unsigned index_check_core(const char *root, char *ex, size_t exn, int progress, unsigned *core) {
    char ip[2100]; snprintf(ip, sizeof ip, "%s/index.tsv", root); FILE *f = fopen(ip, "r");
    if (core) *core = 0;
    if (!f) { if (exn) snprintf(ex, exn, "index.tsv 打不开：%s", errno == ENOENT ? "不在" : strerror(errno)); if (core) *core = 1; return 1; }   // 审计 P2-7：原先只记缺 1 个、例子为空
    char line[4600], p[6800]; unsigned miss = 0; struct stat st; if (exn) ex[0] = 0;
    uint64_t total = 0, done = 0;
    if (progress) { while (fgets(line, sizeof line, f)) total++; rewind(f); PHASE(3, 0, total); }
    while (fgets(line, sizeof line, f)) {
        if (progress && (++done % 256 == 0 || done == total)) PHASE(3, done, total);
        char *t = strchr(line, '\t'); if (!t) continue; *t = 0; long long want = atoll(t + 1);
        snprintf(p, sizeof p, "%s/%s", root, line);
        int gone = stat(p, &st) != 0;
        if (gone || !S_ISREG(st.st_mode) || st.st_size != want) { if (!miss++ && exn) snprintf(ex, exn, "%s（%s）", line, gone ? "不在" : !S_ISREG(st.st_mode) ? "不是文件" : "大小不符");
            // 只有真缺（不在 / 不是文件）才算核心缺件挡起局；在、只是大小和清单不符的按「部分可玩」：解包记 .partial、起局问玩家仍要启动、下次推送照补
            //   （维护者 11-17：「校验不过就整个拒绝」一律撤掉）
            if (core && (gone || !S_ISREG(st.st_mode)) && s2a_pack_core_path(line)) (*core)++; }
    }
    if (ferror(f)) { if (exn) snprintf(ex, exn, "index.tsv 读失败：%s", strerror(errno)); miss++; if (core) (*core)++; }
    fclose(f); return miss;
}
// root/src 整份拷成 root/dst（先写 dst.tmp 再改名）；返回 0 成功
static int copy_atomic(const char *root, const char *src, const char *dst) {
    char a[2100], b[2100], t[2110]; snprintf(a, sizeof a, "%s/%s", root, src); snprintf(b, sizeof b, "%s/%s", root, dst); snprintf(t, sizeof t, "%s.tmp", b);
    FILE *in = fopen(a, "rb"); if (!in) return -1; FILE *out = fopen(t, "wb"); if (!out) { fclose(in); return -1; }
    char buf[65536]; size_t k; int bad = 0; while ((k = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, k, out) != k) { bad = 1; break; }
    bad |= ferror(in); fclose(in); bad |= fclose(out) != 0;
    if (bad || rename(t, b)) { unlink(t); return -1; } return 0;
}
// 写 root/name = s（先写 name.tmp 再改名；11-01 审计 P2-16：原先 fopen("w") 先截断再写，中途被杀 / 写失败旧标记就丢了）；返回 0 成功
static int write_atomic(const char *root, const char *name, const char *s) {
    char p[2100], t[2110]; snprintf(p, sizeof p, "%s/%s", root, name); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    int bad = fputs(s, f) < 0; bad |= fclose(f) != 0;
    if (bad || rename(t, p)) { unlink(t); return -1; } return 0;
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
// 读 manifest.json 里 "index_sha256":"…"（执行器写的定长格式，只取这一个键）
static int manifest_sha(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return -1; char b[4096]; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k] = 0;
    char *p = strstr(b, "\"index_sha256\""); if (!p || !(p = strchr(p + 14, '"'))) return -1;
    char *e = strchr(++p, '"'); if (!e || (size_t)(e - p) >= n) return -1;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0; return 0;
}
// 本款 game.img 是玩家放进「放这里导入」认领的、不是电脑推送的：认领写好清单后同写一份 .imported（内容同清单）。解包判 IMG 坏时
//   .imported 与当前清单的 index_sha256 相同才算导入来的（之后电脑推送会先置空再重写清单，号就对不上），码换成 ios_import_img_corrupt、
//   下一步说「删掉这款重新导入」而不是「从电脑重装续传」。写不成只是下一步按推送说，不算认领失败
static void mark_imported(const char *root, const char *mf) {
    char p[2400]; snprintf(p, sizeof p, "%s/.imported", root); FILE *f = fopen(p, "w"); if (f) { fputs(mf, f); fclose(f); }
}
static int came_from_import(const char *root) {
    char p[2400], a[160], b[160]; snprintf(p, sizeof p, "%s/.imported", root); if (manifest_sha(p, a, sizeof a)) return 0;
    snprintf(p, sizeof p, "%s/manifest.json", root); return !manifest_sha(p, b, sizeof b) && !strcmp(a, b);
}
// 玩家导入（s2a_import_gate）用：每批解完回调一次（已解批数、总批数），失败原因留给提示页
void (*s2a_pack_apply_progress)(int done, int total);
static char g_apply_fail[2400], g_apply_code[48];
const char *s2a_pack_apply_failure(void) { return g_apply_fail; }
// 失败给码（11-11 维护者「补上说人话的原因」）：码表在 launchers/shared/experience.v1.json errors.items（下一步在 pack/jobs/errors.py RECOVERY），
//   屏上按码显示标题 + 下一步，原文（s2a_pack_apply_failure）只进日志 / 诊断包。from_img = 失败出在从 game.img 解小文件那步。规格里没有的情形一律 ios_install_unknown
const char *s2a_install_code(int rc, int err, int truncated, int from_img) {
    if (!rc) return "";
    if (rc == -12 || (rc == -4 && (err == EACCES || err == EPERM))) return "ios_install_dir_unwritable";   // 写不进是权限（容器里归别的用户的目录，#569）：重试没用，给「导出存档 → 重装」那条
    if (err == ENOSPC || err == EDQUOT) return "ios_install_no_space";
    if (truncated || rc == -2) return "ios_install_pack_truncated";   // -2：推送包头 / 校验和不对，同样是没传对，重推补齐
    if (rc == -11) return "ios_install_img_format";
    if (rc == -10 || (from_img && rc == -3)) return "ios_install_img_corrupt";
    if (rc == -8) return "ios_install_files_missing";
    if (rc == -6 || rc == -7) return "ios_install_manifest_bad";
    if (rc == -4) return "ios_install_write_failed";
    return "ios_install_unknown";
}
const char *s2a_pack_apply_code(void) { return g_apply_code; }
// 返回进程退出码（0 成功）；docs = Documents 目录
// ---- 资源包里「App 自己的、但缺写权限」的目录 / 文件补回属主读写（D-A76N，11-17 测试设备复现）：
//   电脑端把 0555 的目录拷进手机后，那条目录仍归 App（uid 501）但没有写位：afc 在里面建 / 删 / 覆盖全被拒（Permission denied），devicectl 却写得进，
//   玩家看到「删掉重写也写不进」。属主是自己就能 chmod 回来，不必删除重装；归别的用户的（#569，root）不动，照旧走下面的「目录 App 写不进」。
//   只补属主位（目录 u+rwx、普通文件 u+rw），不碰组 / 其他位。docs（本款文档根）只看自己这一层，资源包根整棵查。返回补了几个
static unsigned g_repair_n, g_repair_self;
static int repair_one(const char *p, const struct stat *st, int flag, struct FTW *w) {
    (void)w; if (flag == FTW_NS || st->st_uid != g_repair_self) return 0;
    mode_t m = st->st_mode & 07777, want = S_ISDIR(st->st_mode) ? (m | S_IRWXU) : S_ISREG(st->st_mode) ? (m | S_IRUSR | S_IWUSR) : m;
    if (want != m && !chmod(p, want)) g_repair_n++;
    return 0;
}
#ifdef S2A_UNIT_TEST
unsigned s2a_pack_repair_self_override;   // 单测：非 0 时把它当「App 自己」（模拟 #569 条目归别的用户：测试进程改不了属主，只能把自己当外人）
#endif
static unsigned repair_self(void) {
#ifdef S2A_UNIT_TEST
    if (s2a_pack_repair_self_override) return s2a_pack_repair_self_override;
#endif
    return (unsigned)getuid();
}
unsigned s2a_pack_repair_modes_as(const char *docs, const char *root, unsigned self_uid) {
    g_repair_n = 0; g_repair_self = self_uid; struct stat st;
    if (!lstat(docs, &st) && S_ISDIR(st.st_mode)) repair_one(docs, &st, FTW_D, NULL);
    if (!lstat(root, &st) && S_ISDIR(st.st_mode)) nftw(root, repair_one, 32, FTW_PHYS);
    return g_repair_n;
}
// App 启动时整棵补（ios/app.m img_scan_bg，在任何解包之前）：推送一报「写不进」电脑端就停，走不到解包开头那次补，
//   玩家只能删 App 重装（D-A76N 第二回：照提示删了 Mac 上的游戏重出，还是写不进）。容器 Documents 与 Library 下 S2Arm 目录各跑一次，
//   只补 App 自己名下的；归别的用户的（#569）照旧不动、照旧报
unsigned s2a_pack_repair_tree(const char *dir) {
    g_repair_n = 0; g_repair_self = repair_self(); struct stat st;
    if (dir && !lstat(dir, &st) && S_ISDIR(st.st_mode)) nftw(dir, repair_one, 32, FTW_PHYS);
    return g_repair_n;
}
static void img_shed(const char *root, const char *logp);   // 截掉导入本体里重复的派生件（定义在着色器更新包那段之后）
// 同一进程里一次只有一路在解包（11-19 Duo：游戏库 s2a_shell_install_game 与资源导入闸门 gate_thread 各自对同一款起了解包。游戏库那路解完、删了 .incoming，
//   闸门那路随后打开 1795095613_002.tar 回 errno 2、按失败收场，起局 60 秒没就绪；重跑没撞上就正常）。两路读同一个 .incoming、写同一棵资源包，
//   还共用本文件的失败现场（g_apply_fail / g_untar_err），并发本身就不对：整段串行。后到的一路等前一路做完再看——待解包、压缩块都没了，
//   .applied 也已是本份清单的号 → 前一路已经解开，不再重解，直接算成功；否则（前一路失败 / 又推来了新包）照常再解一遍
static pthread_mutex_t g_apply_mu = PTHREAD_MUTEX_INITIALIZER;
static int pack_apply_locked(const char *docs);
static int applied_by_other(const char *docs) {   // 待解包已空、.applied = 清单号 → 1
    char root[2048], p[2200], want[200], got[200] = ""; s2a_pack_root(root, sizeof root, docs);
    snprintf(p, sizeof p, "%s/.incoming", root);
    if (has_zchunks(p)) return 0;
    DIR *d = opendir(p); if (d) { struct dirent *e; int tar = 0; while (!tar && (e = readdir(d))) { size_t l = strlen(e->d_name); tar = l > 4 && !strcmp(e->d_name + l - 4, ".tar") && strncmp(e->d_name, "._", 2); } closedir(d); if (tar) return 0; }
    snprintf(p, sizeof p, "%s/manifest.json", root); if (manifest_sha(p, want, sizeof want)) return 0;
    snprintf(p, sizeof p, "%s/.applied", root); FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t k = fread(got, 1, sizeof got - 1, f); got[k] = 0; got[strcspn(got, "\r\n")] = 0; fclose(f);
    return !strcmp(got, want);
}
int s2a_pack_apply(const char *docs) {
    if (pthread_mutex_trylock(&g_apply_mu)) {   // 另一路正在解：等它做完
        LOG("资源包收推送：另一路正在解开，等它做完");
        pthread_mutex_lock(&g_apply_mu);
        if (applied_by_other(docs)) { g_apply_fail[0] = 0; g_apply_code[0] = 0; pthread_mutex_unlock(&g_apply_mu); LOG("资源包收推送：完成（另一路刚解开，不再重解）"); return 0; }
    }
    int rc = pack_apply_locked(docs);
    pthread_mutex_unlock(&g_apply_mu);
    return rc;
}
static int pack_apply_locked(const char *docs) {
    char root[2048], inc[2100], logp[2100]; s2a_pack_root(root, sizeof root, docs); snprintf(inc, sizeof inc, "%s/.incoming", root); snprintf(logp, sizeof logp, "%s/unpack.log", docs);
    if (pack_in_bundle(root, "解开资源包")) return -12;   // 同「目录写不进」的码
    // 多款容器第一次装一款时本款文档根 Documents/games/<游戏号>/ 还不在（资源包在 Library 下）：先建出来，解包日志才写得下。
    //   原先建不了日志、解开了也没有完成行，电脑端（tools/push_pack.py 只认日志完成行）白等 180 秒后误报「解包没有开始」。建不了时 mkdirs 记的现场由下一行清掉，失败照旧走下面「unpack.log 建不了」那句
    mkdirs(logp);
    uint64_t t0 = now_ns(); unsigned ntar = 0, nf = 0; uint64_t nb = 0; int rc = 0, from_img = 0; char what[2300] = ""; g_fail_errno = 0; g_untar_trunc = 0; g_untar_err[0] = 0;
    { unsigned fixed = s2a_pack_repair_modes_as(docs, root, repair_self()); if (fixed) LOG("资源包：%u 个目录 / 文件是 App 自己的但缺写权限，已补回属主读写（D-A76N）", fixed); }
    // 本款目录 / 资源包 / 待解包目录 App 写不进（#569：玩家机上这一串归 root、0755，App 是 501）：一个包都不解，先把真实原因说清楚，
    //   不再去解包、报一句含糊的「解 xxx.tar」。不迁移（App 改不了删不掉，只有删除重装能清）：给码 ios_install_dir_unwritable，界面说「先导出存档再重装」
    { const char *dirs[] = {docs, root, inc}; struct stat ds;
      for (int i = 0; i < 3 && !rc; i++) if (!stat(dirs[i], &ds) && S_ISDIR(ds.st_mode) && access(dirs[i], W_OK)) {
          rc = -12; snprintf(what, sizeof what, "目录 App 写不进：%s（属主 uid %u、权限 %o，App 是 uid %u），App 改不了也删不掉（只有删除重装 S2Arm 能清）", dirs[i], (unsigned)ds.st_uid, (unsigned)(ds.st_mode & 07777), (unsigned)getuid()); } }
    // 压缩传输的 game.img 块先解（img_unzst）：后面从 game.img 解小文件、核对都要整份 game.img
    if (!rc) { int r = img_unzst(root, inc, logp, &ntar, &nb); if (r) { rc = r; snprintf(what, sizeof what, "解压缩传输的 game.img 块"); } }   // 解开的块计进「解开 N 个包」
    // 待解包只由推送端（tools/push_pack.py）写进 .incoming：不再跳过访达拷来的 `._` 附属文件、不设个数上限（iOS 装机剃刀 #20，随 #28）
    DIR *d = rc ? NULL : opendir(inc); char **names = NULL; int nn = 0, cap = 0;
    if (d) { struct dirent *e; while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".tar") && strncmp(e->d_name, "._", 2)) {
        if (nn == cap) { char **grown = realloc(names, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *names); if (!grown) break; names = grown; }
        names[nn++] = strdup(e->d_name); } } closedir(d); }
    if (nn) qsort(names, (size_t)nn, sizeof *names, name_cmp);
    if (s2a_pack_apply_progress) s2a_pack_apply_progress(0, nn);
    if (nn) PHASE(1, 0, (uint64_t)nn);
    for (int i = 0; i < nn && !rc; i++) {
        char t[2400]; snprintf(t, sizeof t, "%s/%s", inc, names[i]);
        unsigned f0 = nf; uint64_t b0 = nb, t1 = now_ns();
        rc = s2a_untar(t, root, &nf, &nb);
        // 截短的推送包（推送中途被杀留下的）永远解不开：删掉它、它后面所有待解包和推送记录 .pushed，下次部署执行器只按 index.applied.tsv 补推（10-20 测试设备2 巫师3 同一个残包连红 4 局）。
        //   只删残包不够（10-21 测试出包线）：执行器见 .incoming 里还有包就把 .pushed 记的文件当「已推」，残包里的文件不补推；后面的包解完照写 index.applied.tsv = 全部目标，
        //   机上缺文件 / 留半截文件却记成已应用。待解包全删 = 机上状态退回 index.applied.tsv 那一刻，执行器补推的就是缺的全部
        if (rc) { int del = 0;
            if (g_untar_trunc && !unlink(t)) { del = 1; for (int j = i + 1; j < nn; j++) { char u[2400]; snprintf(u, sizeof u, "%s/%s", inc, names[j]); del += !unlink(u); }
                char pp[2100]; snprintf(pp, sizeof pp, "%s/.pushed", root); unlink(pp); }
            snprintf(what, sizeof what, "解 %s%s", names[i], del ? "（截短包：已删它与其后待解包、推送记录，下次部署按已应用索引补推）" : ""); }
        else { unlink(t); ntar++; if (s2a_pack_apply_progress) s2a_pack_apply_progress(i + 1, nn); PHASE(1, (uint64_t)(i + 1), (uint64_t)nn); log_line(logp, "进度：解开 %s（%u/%d）%u 个文件 %llu MB（%.1f 秒）", names[i], i + 1, nn, nf - f0, (unsigned long long)((nb - b0) >> 20), (now_ns() - t1) / 1e9); }
    }
    if (!rc) { int r = img_extract(root, &nf, &nb); if (r) { rc = r; from_img = 1; snprintf(what, sizeof what, "从 game.img 解小文件（%s）", r == -11 ? "格式号不认识，请更新 S2Arm" : r == -10 ? "IMG 坏或没传完" : "写失败"); } }
    if (!rc) img_shed(root, logp);   // 玩家导入的完整 IMG：派生件解出来以后，截掉本体尾部那份重复的（0.6.0）
    unsigned partial = 0; char pex[2200] = "";
    if (!rc) { char ex[2200]; unsigned core = 0, miss = index_check_core(root, ex, sizeof ex, 1, &core);
        if (miss && !core) { partial = miss; snprintf(pex, sizeof pex, "%s", ex); }   // v0.3.1 缺件照玩：只缺非核心文件 → 记部分可玩（.partial），不记已应用，执行器下次照样补推
        else if (miss) { rc = -8; snprintf(what, sizeof what, "核对：index.tsv 里 %u 个文件机上缺或大小不符（例：%s），不记已应用，执行器下次按旧的已应用索引补推", miss, ex); } }   // 核对在清旧文件之前：核不过不删任何文件
    // 程序描述 exefs/main.npdm 不在 / 是空的：物理上起不来，游戏库就绪判据（applied_ready / partial_ready）也要它。原先只按 index.tsv 核，清单里没列它时照报「解开了」，
    //   库却一直判「等解开」、每秒重解一次、失败原因为空（#1059 D-PB7S：installing → failed(reason nil) 循环、Play 被拒）。两边判据合成一条：这里核不过就按缺文件失败、说清缺的是它
    if (!rc) { char np[2100]; struct stat ns; snprintf(np, sizeof np, "%s/exefs/main.npdm", root);
        if (stat(np, &ns) || !S_ISREG(ns.st_mode) || ns.st_size <= 0) { rc = -8; snprintf(what, sizeof what, "核对：程序描述 exefs/main.npdm %s，不记已应用，请回电脑重新装到设备", stat(np, &ns) ? "不在" : !S_ISREG(ns.st_mode) ? "不是文件" : "是空的"); } }
    // 本体 game.img 在就核长度（D-YGEC：本体只剩 5 MB 也照写 .applied，游戏库判就绪、点开服务层才报没传完）：同快判 img_whole_or_absent，核不过按缺文件失败
    if (!rc && !img_whole_or_absent(root)) { rc = -8; snprintf(what, sizeof what, "核对：本体 game.img 不完整（实际长度与头里写的整个文件长不符），不记已应用，请回电脑重新装到设备"); }
    if (!rc) { unsigned nd = 0; uint64_t db = 0; int r2 = prune(root, &nd, &db);   // 清旧文件：资源包下不在 index.tsv 里的（旧版分卷等）
        if (r2) { rc = r2; snprintf(what, sizeof what, "清旧文件（读 index.tsv）"); }
        else if (nd) log_line(logp, "进度：清掉不在 index.tsv 里的旧文件 %u 个 %llu MB", nd, (unsigned long long)(db >> 20)); }
    for (int i = 0; i < nn; i++) free(names[i]);
    free(names);
    char sha[200] = "";
    if (!rc) { char mp[2100]; snprintf(mp, sizeof mp, "%s/manifest.json", root);
        if (manifest_sha(mp, sha, sizeof sha)) { rc = -6; snprintf(what, sizeof what, "读 manifest.json 的 index_sha256"); }
        else if (partial) {   // 部分可玩：不存 index.applied.tsv（它说「机上都有」，执行器会以为齐了、不再补推缺的）、不写 .applied；写 .partial = 清单 sha、缺几个、例子
            char pv[2600]; snprintf(pv, sizeof pv, "%s\n%u\n%s\n", sha, partial, pex);
            if (write_atomic(root, ".partial", pv)) { rc = -4; g_fail_errno = errno; snprintf(what, sizeof what, "写 .partial"); }
            else { char tp[2100]; snprintf(tp, sizeof tp, "%s/.transfer", root); unlink(tp); } }
        else if (copy_atomic(root, "index.tsv", "index.applied.tsv")) { rc = -4; g_fail_errno = errno; snprintf(what, sizeof what, "存 index.applied.tsv"); }   // 「机上实际有什么」：执行器据它算补推
        else if (write_atomic(root, ".applied", sha)) { rc = -4; g_fail_errno = errno; snprintf(what, sizeof what, "写 .applied"); }
        else if (write_atomic(root, ".applied_by_shell", sha)) LOG("【黄】资源包来源标记 .applied_by_shell 写不进（游戏库会把还没收到程序的这款当成 0.5 老款）");   // 0.6 起解开的资源包（游戏库据此分清 0.5 老款与还没收到 .s2arm 的新款：shell Catalogue.swift）
        else { char tp[2100], pp[2100]; snprintf(tp, sizeof tp, "%s/.transfer", root); unlink(tp); snprintf(pp, sizeof pp, "%s/.partial", root); unlink(pp); } }   // 解开了：这次推送的传输清单（tools/push_pack.py 写）用完，游戏库不再按它画进度；齐了就撤掉部分可玩标记
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&now));
    FILE *lf = fopen(logp, "a"); int lerr = lf ? 0 : errno;
    // 失败原因不依赖 unpack.log 打得开（#540：游戏目录 Documents/games/<游戏号>/ 不在时 unpack.log 也建不了，原先原因只在 lf 打开时才填，
    //   host.log 那一行「解开 … 没成：」后面是空的、又指去一个不存在的 unpack.log）：先填原因，日志打不开再把这一句接上
    g_apply_fail[0] = 0; snprintf(g_apply_code, sizeof g_apply_code, "%s", s2a_install_code(rc, g_fail_errno, g_untar_trunc, from_img));
    if (!strcmp(g_apply_code, "ios_install_img_corrupt") && came_from_import(root)) snprintf(g_apply_code, sizeof g_apply_code, "ios_import_img_corrupt");
    if (rc) { int k = snprintf(g_apply_fail, sizeof g_apply_fail, "%s（返回 %d）%s%s", what, rc, g_untar_err[0] ? "；现场：" : "", g_untar_err);   // 现场进 g_apply_fail：界面红行 / 诊断都带上 errno 原文（D-4VAX）
        if (!lf && k > 0 && (size_t)k < sizeof g_apply_fail) snprintf(g_apply_fail + k, sizeof g_apply_fail - (size_t)k, "；unpack.log 建不了：%s %s", logp, strerror(lerr)); }
    if (lf) {
        if (rc) fprintf(lf, "%s 解包失败：%s 返回 %d（已解 %u 个包 %u 个文件）%s%s\n", ts, what, rc, ntar, nf, g_untar_err[0] ? "；现场：" : "", g_untar_err);
        else if (partial) fprintf(lf, "%s 解开 %u 个包 %u 个文件 %llu MB，缺 %u 个非必需文件（例：%s），部分可玩 = %s（%.1f 秒）\n", ts, ntar, nf, (unsigned long long)(nb >> 20), partial, pex, sha, (now_ns() - t0) / 1e9);
        else fprintf(lf, "%s 解开 %u 个包 %u 个文件 %llu MB，.applied = %s（%.1f 秒）\n", ts, ntar, nf, (unsigned long long)(nb >> 20), sha, (now_ns() - t0) / 1e9);
        fclose(lf);
    }
    if (rc) LOG("资源包收推送：失败（%s）：%s", g_apply_code, g_apply_fail); else LOG("资源包收推送：完成");   // g_apply_fail 已带现场（哪一步、哪个路径、errno 原文）
    return rc ? 1 : 0;
}

// ---- 玩家首次打开时从「文件」导入资源（用户体验线 10-23，原生层线审：后台线程解包、解完提示重新打开、不同进程内接着进游戏）----
// 状态：先改名（Documents/资源包 不在、且恰好一个名字以「资源包」结尾的目录 → 改名成「资源包」，同卷原子；多个不猜）→ 有 .incoming/*.tar = 待导入 → 有 manifest.json = 就绪 → 否则没有资源
enum { S2A_IMPORT_READY = 0, S2A_IMPORT_PENDING = 1, S2A_IMPORT_NONE = 2, S2A_IMPORT_BROKEN = 3, S2A_IMPORT_PARTIAL = 4 };   // 4：只缺非核心文件，玩家确认后照玩（v0.3.1）
// 闸门下一步（s2a_import_decide）与它的节拍：稳住 3 秒才导入、1 分钟内变过算正在拷、推送没完且 3 分钟没动算停住
enum { S2A_GATE_READY, S2A_GATE_IMPORT, S2A_GATE_WAIT, S2A_GATE_STALLED, S2A_GATE_NONE, S2A_GATE_BROKEN };
enum { S2A_GATE_STALL_S = 90 };   // 停住：同游戏库 transferStall（S2ArmShellHostModel.swift，iOS 装机剃刀 #32：原先闸门 180 秒、库 90 秒两套）
struct s2a_gate_watch { int n; uint64_t fp; time_t changed; };
// 资源包完整性（10-26 测试设备2：装机拷到一半被 App 改名，留下有 manifest.json、缺 ctl/control.nacp 的「资源包」，原先只凭 manifest 判就绪，
//   游戏起来读不到数据 Break、App 退出像闪退）：按 index.tsv（路径<TAB>字节<TAB>sha256）逐个核在不在、大小对不对，返回不符个数，first 记第一个。
//   index.tsv 由 pack/pack.py 与 manifest.json 同写、出包保证在：不在 / 打不开 / 读不了都按不完整（审计 P2-7：原先打不开当「老包不核」判就绪），first 记 index.tsv 与原因
static int g_pack_bad; static char g_pack_first[512];
// 说明页正文（app.m s2a_ios_show_pack_help）。again = 玩家刚按过「重新检查」、还是不行：开头先说一句，不然整页原样重画，玩家以为按钮没生效（审计 P2-2）
void s2a_pack_help_detail(int st, int again, char *d, size_t n) {
    const char *pre = again ? S2A_UI(S2A_UI_PACK_AGAIN) : "";
    // 缺哪个文件（g_pack_first）只进日志（s2a_import_state 已打），不上屏（11-02 维护者：index.tsv 这类内部词玩家看不懂）
    snprintf(d, n, "%s%s", pre, st == S2A_IMPORT_PARTIAL ? S2A_UI(S2A_UI_PACK_PARTIAL_DETAIL) : st == S2A_IMPORT_BROKEN ? S2A_UI(S2A_UI_PACK_BROKEN_DETAIL) : S2A_UI(S2A_UI_PACK_NONE_DETAIL));
}
int s2a_pack_check(const char *root, char *first, size_t n) { return (int)index_check(root, first, first ? n : 0, 0); }
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
// 待解包里至少有一个条目（审计 P1-169 余项）：头一个 512 字节块不是全零。只有归档结束块的空 tar（1024 字节全零，推送端残批的「盖子」）解开什么都不放，
//   不算一次新推送——原先迁移见 .incoming 里有 .tar 就拿旧位置换掉新位置，空包把已经能玩的新位置换没了
static int tar_has_entry(const char *path) {
    uint8_t b[512]; int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b); close(fd); if (n != (ssize_t)sizeof b) return 0;
    for (size_t i = 0; i < sizeof b; i++) if (b[i]) return 1;
    return 0;
}
static int applied_ready(const char *root);
static int partial_ready(const char *root);
int s2a_import_state(const char *docs) {
    char root[2048]; struct stat st; s2a_pack_root(root, sizeof root, docs);
    int have = !stat(root, &st) && S_ISDIR(st.st_mode);
    // 「资源包」不在 = 没有资源。原先还会把 Documents 下唯一一个「<游戏名>资源包」改名成「资源包」、把空的「资源包」挪开（玩家用访达拷整个文件夹那条路，
    //   10-26）：v0.3.0 起单一 IMG 交付、单款壳不出，说明页只教「装到 iPhone」与「文件」里放 game.img，这条路连同查询里的改名副作用一起删（iOS 装机剃刀 #28）
    if (!have) return S2A_IMPORT_NONE;
    // 先用游戏库那条快判（applied_ready）：解开时核对过每个文件、写下的 .applied 等于当前清单、程序描述在 = 就绪，不再逐文件核。
    //   原先闸门先看 .incoming、库先看 .applied，顺序相反：机上留一个包且清单没变时库说能玩、闸门判待导入 + 在传，等到「停住了」进不去；
    //   每次起局还要对同一目录逐文件核一遍（Play 时主线程 s2a_import_needs_ui 再核一遍）。逐文件核只留在没有 .applied 的路径与游戏 Break 时（iOS 装机剃刀 #27 / #38 / #39）
    if (applied_ready(root)) return S2A_IMPORT_READY;
    if (partial_ready(root)) return S2A_IMPORT_PARTIAL;
    char inc[2100]; snprintf(inc, sizeof inc, "%s/.incoming", root); int ntar = 0;
    DIR *d = opendir(inc); if (d) { struct dirent *e; while ((e = readdir(d))) if (ends_with(e->d_name, ".tar") && strncmp(e->d_name, "._", 2)) ntar++; closedir(d); }
    if (ntar || has_zchunks(inc)) return S2A_IMPORT_PENDING;   // 压缩传输的 game.img 块（img_unzst）同待解包
    char mf[2100]; snprintf(mf, sizeof mf, "%s/manifest.json", root);
    if (stat(mf, &st)) return S2A_IMPORT_NONE;
    unsigned core = 0; g_pack_bad = (int)index_check_core(root, g_pack_first, sizeof g_pack_first, 0, &core);
    if (g_pack_bad && !core) { LOG("资源导入：缺 %d 个非必需文件（如 %s），部分可玩：玩家确认后照玩", g_pack_bad, g_pack_first); return S2A_IMPORT_PARTIAL; }
    if (g_pack_bad) { LOG("资源导入：资源包不完整，缺 / 不对 %d 个文件（如 %s，其中核心 %u 个）", g_pack_bad, g_pack_first, core); return S2A_IMPORT_BROKEN; }
    if (!img_whole_or_absent(root)) { snprintf(g_pack_first, sizeof g_pack_first, "game.img"); g_pack_bad = 1; return S2A_IMPORT_BROKEN; }   // D-YGEC：本体截短 = 资源不完整，不起局
    if (img_needs_apply(root)) return S2A_IMPORT_PENDING;   // game.img 到了、小文件还没解：先解开再就绪
    return S2A_IMPORT_READY;
}
// ---- App 容器里属主不是 App 自己的条目（#569 / 维护者 11-15 定：不迁移、不驻留、不靠电脑）：只清点、不动。
//   这些条目（玩家机上归 root、0755）App 改不了也删不掉（删目录里的条目要对那个目录本身有写权限），电脑端 AFC 同样是 App 用户身份、也删不掉；
//   只有卸载 App 能清。所以宿主只做两件事：诊断包 / 日志里记清楚（个数、占用、最早那条的路径 / 时间 / 属主——以后来件能定是哪种安装方式或迁移造成的），
//   以及真有影响时（装 / 起局写不进、或占用超过阈值）告诉玩家「先导出存档 → 删除重装 S2Arm → 导回存档」（ios/app.m）
static struct { unsigned n, self; uint64_t bytes; time_t first_t; unsigned first_uid; char first[1200]; size_t base; } g_fs;
static int foreign_one(const char *p, const struct stat *st, int flag, struct FTW *w) {
    (void)w; if (flag == FTW_NS || st->st_uid == g_fs.self) return 0;
    g_fs.n++; if (S_ISREG(st->st_mode)) g_fs.bytes += (uint64_t)st->st_blocks * 512;
#if defined(__APPLE__)
    time_t t = st->st_birthtimespec.tv_sec ? st->st_birthtimespec.tv_sec : st->st_mtime;
#else
    time_t t = st->st_mtime;
#endif
    if (!g_fs.first[0] || t < g_fs.first_t) { g_fs.first_t = t; g_fs.first_uid = (unsigned)st->st_uid; snprintf(g_fs.first, sizeof g_fs.first, "%s", p + (strlen(p) >= g_fs.base ? g_fs.base : 0)); }
    return 0;
}
// home = App 容器根（Documents 的上一级）。返回条目数；*bytes = 其中普通文件实占；line（可空）= 一句可进日志 / 诊断的读数
//   self_uid：当成「App 自己」的 uid（正式路径 getuid()；单测传别的值，把自己建的条目当外人数）
unsigned s2a_foreign_scan_as(const char *home, unsigned self_uid, uint64_t *bytes, char *line, size_t n) {
    memset(&g_fs, 0, sizeof g_fs); g_fs.base = strlen(home); g_fs.self = self_uid;
    nftw(home, foreign_one, 32, FTW_PHYS);
    if (bytes) *bytes = g_fs.bytes;
    if (line && n) {
        if (!g_fs.n) snprintf(line, n, "容器内属主不是 App（uid %u）的条目：0", self_uid);
        else { char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", gmtime(&g_fs.first_t));
            snprintf(line, n, "容器内属主不是 App（uid %u）的条目：%u 个、文件占 %llu MB；最早一条 <容器>%s 建于 %s UTC、属主 uid %u", self_uid, g_fs.n,
                     (unsigned long long)(g_fs.bytes >> 20), g_fs.first, ts, g_fs.first_uid); } }
    return g_fs.n;
}
#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif
static void remove_tree(const char *p) {   // 删一棵树（自己建的、删得掉的）
    struct stat st; if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) { DIR *d = opendir(p); if (d) { struct dirent *e; while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2400]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); remove_tree(c); } closedir(d); } rmdir(p); }
    else unlink(p);
}
unsigned s2a_foreign_scan(const char *home, uint64_t *bytes, char *line, size_t n) { return s2a_foreign_scan_as(home, (unsigned)getuid(), bytes, line, n); }

// ---- 资源包挪出 Documents（v0.3.0，11-11 真机复现：Documents 里几万个资源包文件拖住「文件」App 与系统选择器的「最近项目」，一直「正在加载」）：
//   每款 Documents/games/<游戏号>/资源包 → Library/Application Support/S2Arm/games/<游戏号>/资源包（s2a_docs.h s2a_pack_root）。
//   同一个容器同一个卷：rename(2) 一次挪整个目录，不拷贝不重写，几毫秒；rename 是原子的，迁到一半被杀 = 这一款要么还在旧位置要么已在新位置，下次再判接着迁。
//   存档（S2A/ryujinx）、金手指、unpack.log 都在本款文档根别处，不在挪动范围里。
//   新位置已有（电脑端先推了新位置、或上次迁过）：旧位置是一次新推送（老版本电脑端推的：有待解包 / 清单不同）→ 换掉新位置；
//   否则新位置已就绪（.applied 对得上清单）→ 旧位置是重复的，删掉；新位置没就绪 → 两处都不动、下次再判（不丢任何一份）。
//   docs = 本款文档根 Documents/games/<游戏号>。返回 1 = 挪了 / 删了旧的；0 = 没有旧位置或不是多款容器的文档根；-1 = 新位置没就绪、两处都留着；-2 = 挪失败（原样留着）
static int applied_ready(const char *root);
static char g_migrate_why[600];
const char *s2a_pack_migrate_failure(void) { return g_migrate_why; }
int s2a_pack_migrate_one(const char *docs) {
    char old[2200], neu[2200]; struct stat st;
    s2a_pack_legacy_root(old, sizeof old, docs);
    if (s2a_pack_root(neu, sizeof neu, docs) >= (int)sizeof neu || !strcmp(old, neu)) return 0;   // 单款兼容 / Mac：资源包本来就在文档根下
    if (lstat(old, &st) || !S_ISDIR(st.st_mode)) return 0;
    if (pack_in_bundle(neu, "资源包挪位置（Library/Caches 里还有一份旧的）")) return 0;   // 不挪进只读区；包内那份照用，Caches 旧的不动
    if (!lstat(neu, &st)) {
        // 两处都有：旧位置若是一次「新推送」（老版本电脑端推到了旧位置：.incoming 里有包，或清单不是新位置已应用的那份）→ 它新，换掉新位置；
        //   否则旧位置是重复 / 删到一半的残留：新位置已就绪 → 删旧的；新位置没就绪 → 两处都不动、下次再判（不丢任何一份）
        char inc[2300], omf[2300], nap[2300], osha[200] = "", nsha[200] = ""; snprintf(inc, sizeof inc, "%s/.incoming", old);
        snprintf(omf, sizeof omf, "%s/manifest.json", old); snprintf(nap, sizeof nap, "%s/.applied", neu);
        int pending = 0; DIR *d = opendir(inc); if (d) { struct dirent *e; while (!pending && (e = readdir(d))) if (ends_with(e->d_name, ".tar") && strncmp(e->d_name, "._", 2)) { char tp[2600]; snprintf(tp, sizeof tp, "%s/%s", inc, e->d_name); pending = tar_has_entry(tp); } closedir(d); }
        FILE *f = fopen(nap, "r"); if (f) { size_t k = fread(nsha, 1, sizeof nsha - 1, f); fclose(f); nsha[k] = 0; nsha[strcspn(nsha, "\r\n")] = 0; }
        int differs = !manifest_sha(omf, osha, sizeof osha) && strcmp(osha, nsha);
        if (pending || differs) {
            // 三步改名（同 repair_tree；审计 P1-169：原先先删新位置再改名，改名失败或删到一半被杀，已经能玩的新位置就缺了文件）：
            //   ① 新位置改名挪开留作退路 ② 旧位置改名进来 ③ 成功后才删挪开的那份；② 失败就把 ① 改回原名，两处都不丢
            char keep[2300]; snprintf(keep, sizeof keep, "%s.s2a-old-%llu", neu, (unsigned long long)now_ns());
            if (rename(neu, keep)) { snprintf(g_migrate_why, sizeof g_migrate_why, "把新位置 %s 挪开（换成旧位置的新推送前）：%s", neu, strerror(errno)); LOG("【红】资源包迁移：%s", g_migrate_why); return -2; }
            if (rename(old, neu)) {
                int e = errno, back = rename(keep, neu);
                snprintf(g_migrate_why, sizeof g_migrate_why, "把旧位置的新推送 %s 换到 %s：%s%s", old, neu, strerror(e),
                         back ? "；新位置原来那份也没能改回原名（在 .s2a-old-* 里，文件都在）" : "；新位置原来那份已改回原名");
                LOG("【红】资源包迁移：%s", g_migrate_why); return -2;
            }
            remove_tree(keep);
            LOG("资源包迁移：旧位置是一次新推送（%s），换掉新位置的旧资源包", pending ? "有待解包" : "清单不同");
            return 1;
        }
        if (!applied_ready(neu)) { LOG("资源包迁移：%s 新旧两处都有、新位置还没就绪，都不动，下次再判", docs); return -1; }
        remove_tree(old);
        LOG("资源包迁移：新位置已就绪，删掉旧位置的重复资源包 %s", old);
        return 1;
    }
    char parent[2200]; snprintf(parent, sizeof parent, "%s", neu);
    if (mkdirs(parent)) { snprintf(g_migrate_why, sizeof g_migrate_why, "建 %s 的上级目录失败：%s", neu, g_untar_err); LOG("【红】资源包迁移：%s", g_migrate_why); return -2; }   // 只建到上一级：rename 要目标本身不存在
    uint64_t t0 = now_ns();
    if (rename(old, neu)) { snprintf(g_migrate_why, sizeof g_migrate_why, "把 %s 改名到 %s：%s", old, neu, strerror(errno)); LOG("【红】资源包迁移：%s", g_migrate_why); return -2; }
    LOG("资源包迁移：%s → %s（同盘改名 %.1f ms）", old, neu, (now_ns() - t0) / 1e6);
    return 1;
}
// 进程级 Documents 下每一款都迁：返回挪了几款（失败的计在 *failed）。游戏库挂上时（app.m img_scan_bg）在 #569 修复之后、认领 IMG 之前做
int s2a_pack_migrate(const char *proc_docs, int *failed) {
    char games[2048]; snprintf(games, sizeof games, "%s/games", proc_docs); if (failed) *failed = 0;
    DIR *d = opendir(games); if (!d) return 0;
    int moved = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char docs[2300]; snprintf(docs, sizeof docs, "%s/%s", games, e->d_name);
        int r = s2a_pack_migrate_one(docs);
        if (r > 0) moved++; else if (r == -2 && failed) (*failed)++;
    }
    closedir(d);
    return moved;
}

// ---- 玩家把 IMG 放进「文件」里 S2Arm 的「放这里导入」文件夹（v0.3.0，自签 / 侧载玩家）：s2a_img_scan 扫这个文件夹（往下两层，跳过隐藏目录），
//   认文件开头的魔数（不看扩展名，改了名也认）。长度不等于头里的整个文件长 = 还在拷，先不动下次再看；认出来就读头里的游戏号，
//   同卷改名挪到本款资源包根 Library/Application Support/S2Arm/games/<游戏号>/资源包/game.img（s2a_pack_root；文件夹不在就建，玩家不用知道程序号），写 index.tsv（game.img 一行 + 原有的直连库 / 覆盖层行）
//   与清单 manifest.json（version 3、资源包号取头）、作废 .applied——之后就是「待导入」，游戏库 / 闸门照常解小文件（s2a_pack_apply）
static char g_img_err[600], g_img_code[48];
// 这次认领里不带着色器库的那个文件名（最近一个；空 = 没有）：游戏库上提示一句怎么换（ios/app.m img_scan_report；D-QW2Q / D-9ZF9：自签玩家放了资源包文件夹里的 game.img）
static char g_img_noshaders[256];
const char *s2a_img_scan_noshaders(void) { return g_img_noshaders; }
static char g_img_skipped[256];   // 这次扫描跳过（和当前游戏版本不配套）的着色器更新包文件名（空串 = 没有）
const char *s2a_img_scan_skipped(void) { return g_img_skipped; }
const char *s2a_img_scan_failure(void) { return g_img_err; }
// 认领失败给码（同 s2a_install_code）：格式号不认识 = ios_install_img_format，挪 / 写时空间满 = ios_install_no_space，其余 ios_import_img_failed
const char *s2a_img_scan_code(void) { return g_img_code; }
// ---- 着色器更新包（格式号 2，pack/img.py update_pack；导出文件夹里的「<游戏名> 着色器更新包.img」，几百 MB）：只含派生件（shaders/、romfs_overlay/、
//   romfs.s2aidx、game.imgmap），头里的游戏号 + 资源包号指向它配的本体（同款同构建的完整 game.img）。认领：本机这款的 game.img 头里资源包号一致才收；
//   把本体目录表里旧的派生件条目摘掉（不然以后解包会拿旧的盖新的），新派生件直接写成文件、记进 index.tsv，5 GB 本体一个字节不重写。
//   本机没有本体 / 本体不是这一版：物理上套不上，不收，原样留在导入文件夹，当场说清（先导完整包 / 这个更新包配的不是手机上这版）
static int upd_is_extra(const char *rel) { return !strncmp(rel, "shaders/", 8) || !strncmp(rel, "romfs_overlay/", 14) || !strcmp(rel, "romfs.s2aidx") || !strcmp(rel, "game.imgmap"); }
static uint64_t upd_fnv(const char *s, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ULL; } return h ? h : 1; }
typedef struct { char *rel; uint64_t off, len; } upd_ent;
static int upd_read_toc(int fd, uint64_t to, uint64_t tl, upd_ent **out, size_t *n) {   // 原样读目录表（含 romfs.list，img_toc 会跳过它）
    *out = NULL; *n = 0; if (tl < 8 || tl > (64u << 20)) return -1;
    uint8_t *t = malloc(tl); if (!t || pread(fd, t, tl, (off_t)to) != (ssize_t)tl) { free(t); return -1; }
    uint32_t slots; memcpy(&slots, t, 4); size_t pb = 8 + (size_t)slots * 32; if (pb > tl) { free(t); return -1; }
    upd_ent *e = calloc(slots ? slots : 1, sizeof *e); size_t k = 0;
    for (uint32_t i = 0; i < slots; i++) { const uint8_t *q = t + 8 + (size_t)i * 32; uint64_t hh; memcpy(&hh, q, 8); if (!hh) continue;
        uint32_t po, pl; memcpy(&e[k].off, q + 8, 8); memcpy(&e[k].len, q + 16, 8); memcpy(&po, q + 24, 4); memcpy(&pl, q + 28, 4);
        if (pb + po + pl > tl || !toc_path_ok(t + pb + po, pl)) { for (size_t j = 0; j < k; j++) free(e[j].rel); free(e); free(t); return -1; }   // 路径区自检同 img_toc_file：坏表不拿来重写
        e[k++].rel = strndup((const char *)t + pb + po, pl); }
    free(t); *out = e; *n = k; return 0;
}
static void upd_free(upd_ent *e, size_t n) { for (size_t i = 0; i < n; i++) free(e[i].rel); free(e); }
static uint8_t *upd_toc_bytes(upd_ent *e, size_t n, size_t *len) {   // 同 pack/img.py toc_bytes：槽数 = ≥ 2n 的 2 的幂，FNV-1a 开放寻址
    uint32_t slots = 1; while (slots < (n ? n : 1) * 2) slots <<= 1;
    size_t paths = 0; for (size_t i = 0; i < n; i++) paths += strlen(e[i].rel);
    *len = 8 + (size_t)slots * 32 + paths; uint8_t *b = calloc(1, *len); if (!b) return NULL;
    uint32_t cnt = (uint32_t)n; memcpy(b, &slots, 4); memcpy(b + 4, &cnt, 4); uint32_t po = 0;
    for (size_t i = 0; i < n; i++) { size_t pl = strlen(e[i].rel); uint64_t h = upd_fnv(e[i].rel, pl); uint32_t k = (uint32_t)(h & (slots - 1));
        for (;;) { uint8_t *q = b + 8 + (size_t)k * 32; uint64_t cur; memcpy(&cur, q, 8); if (!cur) { uint32_t l32 = (uint32_t)pl;
            memcpy(q, &h, 8); memcpy(q + 8, &e[i].off, 8); memcpy(q + 16, &e[i].len, 8); memcpy(q + 24, &po, 4); memcpy(q + 28, &l32, 4); break; }
            k = (k + 1) & (slots - 1); }
        memcpy(b + 8 + (size_t)slots * 32 + po, e[i].rel, pl); po += (uint32_t)pl; }
    return b;
}
// 摘掉本体 game.img 目录表里的派生件条目：新目录表先写在旧派生件占的那段里（和旧目录表不重叠），再一笔改头（目录表位置 / 长度 / 整长），最后截短。
//   改头前断电：头还指着旧目录表、旧表完好；改头后断电：截短没做只是文件长一点，头里整长已对（读法按头）。→ 新整长；没有派生件条目时原样返回旧整长
static int64_t upd_strip_base(const char *base) {
    int fd = open(base, O_RDWR); if (fd < 0) return -1;
    uint8_t h[136]; int64_t rc = -1; upd_ent *e = NULL; size_t n = 0;
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) goto out;
    uint64_t ro, rl, to, tl, total; memcpy(&ro, h + 96, 8); memcpy(&rl, h + 104, 8); memcpy(&to, h + 112, 8); memcpy(&tl, h + 120, 8); memcpy(&total, h + 128, 8);
    if (upd_read_toc(fd, to, tl, &e, &n)) goto out;
    size_t k = 0; uint64_t end = ro + rl; int removed = 0;
    for (size_t i = 0; i < n; i++) { if (upd_is_extra(e[i].rel)) { free(e[i].rel); removed = 1; continue; } e[k++] = e[i]; if (e[i].off + e[i].len > end) end = e[i].off + e[i].len; }
    n = k;
    if (!removed) { rc = (int64_t)total; goto out; }
    size_t nl; uint8_t *tb = upd_toc_bytes(e, n, &nl); if (!tb) goto out;
    uint64_t nto = (end + 16383) / 16384 * 16384;
    if (nto + nl > to) nto = (total + 16383) / 16384 * 16384;   // 放不进旧派生件那段：接在文件末尾，不截短（极少见：派生件比目录表还小）
    uint64_t ntotal = nto + nl;
    if (pwrite(fd, tb, nl, (off_t)nto) != (ssize_t)nl || fsync(fd)) { free(tb); goto out; }
    free(tb);
    uint8_t nh[24]; memcpy(nh, &nto, 8); uint64_t ntl = nl; memcpy(nh + 8, &ntl, 8); memcpy(nh + 16, &ntotal, 8);
    if (pwrite(fd, nh, sizeof nh, 112) != (ssize_t)sizeof nh || fsync(fd)) goto out;
    if (ntotal < total && ftruncate(fd, (off_t)ntotal)) goto out;
    rc = (int64_t)ntotal;
out:
    if (e) upd_free(e, n); close(fd); return rc;
}
// 玩家导入的完整 IMG（导出格式：派生件追加在本体尾部、进了目录表，pack/img.py export）解完小文件后，截掉本体里那份重复的派生件（0.6.0，体系文档 B / C：
//   着色器库原先在手机上占两份——本体尾部一份、解出来的文件一份）：
//   ① 派生件（shaders/、romfs_overlay/、romfs.s2aidx、game.imgmap）补进 index.tsv（「路径\t字节\t-」）：它们只在目录表里、不在 index.tsv，摘掉目录表条目后清旧文件会删、核对会漏；
//   ② upd_strip_base 摘目录表条目、截短本体（断电安全，见那里）；③ index.tsv 里 game.img 那行的长度、game.imgmap 头里记的整个文件长改成截短后的（服务层 OpenImg 核它，不对就走慢路径）。
//   只对玩家导入的（came_from_import）做；每步断了下次进来接着对齐（②做过了目录表里就没有派生件，①跳过，③照样对齐）。电脑推的本体目录表里本来就没有派生件
static int index_fix(const char *root, const upd_ent *add, size_t nadd, uint64_t img_len) {   // 改写 index.tsv：game.img 那行长度换成 img_len（0 = 不动），补上 add 里不在表里的行
    char ip[2400], tp[2500], line[4600]; snprintf(ip, sizeof ip, "%s/index.tsv", root); snprintf(tp, sizeof tp, "%s.shed", ip);
    FILE *in = fopen(ip, "r"); if (!in) return -1;
    FILE *o = fopen(tp, "w"); if (!o) { fclose(in); return -1; }
    char *have = calloc(nadd ? nadd : 1, 1);
    while (fgets(line, sizeof line, in)) {
        char *tab = strchr(line, '\t'); size_t pl = tab ? (size_t)(tab - line) : strcspn(line, "\n");
        for (size_t i = 0; i < nadd; i++) if (strlen(add[i].rel) == pl && !strncmp(add[i].rel, line, pl)) have[i] = 1;
        if (img_len && pl == 8 && !strncmp(line, "game.img", 8) && tab) { char *rest = strchr(tab + 1, '\t'); fprintf(o, "game.img\t%llu%s", (unsigned long long)img_len, rest ? rest : "\n"); }
        else fputs(line, o);
    }
    for (size_t i = 0; i < nadd; i++) if (!have[i]) fprintf(o, "%s\t%llu\t-\n", add[i].rel, (unsigned long long)add[i].len);
    free(have); fclose(in);
    if (fclose(o) || rename(tp, ip)) { unlink(tp); return -1; }
    return 0;
}
static uint64_t index_img_len(const char *root) {   // index.tsv 里 game.img 那行记的长度；没有 → 0
    char ip[2400], line[4600]; snprintf(ip, sizeof ip, "%s/index.tsv", root); FILE *f = fopen(ip, "r"); uint64_t v = 0; if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "game.img\t", 9)) { v = strtoull(line + 9, NULL, 10); break; }
    fclose(f); return v;
}
static void img_shed(const char *root, const char *logp) {   // 导入来的本体截掉已解成文件的派生件（0.6.0 起通用壳是唯一装法，不再有开关）
    if (!came_from_import(root)) return;
    char ip[2400]; snprintf(ip, sizeof ip, "%s/game.img", root);
    int fd = open(ip, O_RDONLY); if (fd < 0) return;
    uint8_t h[136]; upd_ent *e = NULL; size_t n = 0; uint64_t to, tl, total0;
    int ok = pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h, "S2AIMG01", 8);
    if (ok) { memcpy(&to, h + 112, 8); memcpy(&tl, h + 120, 8); memcpy(&total0, h + 128, 8); ok = !upd_read_toc(fd, to, tl, &e, &n); }
    close(fd); if (!ok) return;
    size_t k = 0; for (size_t i = 0; i < n; i++) { if (upd_is_extra(e[i].rel)) e[k++] = e[i]; else free(e[i].rel); }
    n = k;
    for (size_t i = 0; i < n; i++) { char fp[2600]; struct stat st; snprintf(fp, sizeof fp, "%s/%s", root, e[i].rel);   // 派生件得已经解出来、长度对，才能摘本体里那份
        if (stat(fp, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != e[i].len) { upd_free(e, n); return; } }
    if (n) {
        if (index_fix(root, e, n, 0)) { log_line(logp, "【黄】截本体重复的派生件：补 index.tsv 失败（%s），这次不截", strerror(errno)); upd_free(e, n); return; }
        int64_t nt = upd_strip_base(ip);
        if (nt < 0) { log_line(logp, "【黄】截本体重复的派生件：改本体失败（%s），照原样留着", strerror(errno)); upd_free(e, n); return; }
        log_line(logp, "进度：截掉导入本体里重复的派生件 %zu 个，本体 %llu MB → %llu MB", n, (unsigned long long)(total0 >> 20), (unsigned long long)((uint64_t)nt >> 20));
    }
    upd_free(e, n);
    struct stat st; uint64_t total;
    fd = open(ip, O_RDWR); if (fd < 0) return;
    if (fstat(fd, &st) || pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) { close(fd); return; }
    memcpy(&total, h + 128, 8);
    if (total < (uint64_t)st.st_size && ftruncate(fd, (off_t)total) == 0) st.st_size = (off_t)total;   // 改头后、截短前断了：按头截完
    close(fd);
    if ((uint64_t)st.st_size != total) return;
    if (index_img_len(root) != total) index_fix(root, NULL, 0, total);
    char mp[2400]; snprintf(mp, sizeof mp, "%s/game.imgmap", root);
    int mfd = open(mp, O_RDWR); if (mfd < 0) return;
    uint8_t mh[20]; uint64_t mt;
    if (pread(mfd, mh, sizeof mh, 0) == (ssize_t)sizeof mh && !memcmp(mh, "S2ARIM01", 8) && (memcpy(&mt, mh + 8, 8), mt != total)) {
        if (pwrite(mfd, &total, 8, 8) != 8 || fsync(mfd)) log_line(logp, "【黄】截本体重复的派生件：改 game.imgmap 的整长失败（%s），起局走慢路径", strerror(errno));
    }
    close(mfd);
}
// 作废旧清单但留退路（审计 P1-169）：manifest.json / .applied 先改名成 *.s2a-prev，换本体 / 写派生件这一步失败就改回原名（本体没动，这款照旧在库里能玩）；
//   清单写成了才删退路。原先先删清单：改名进不去或派生件写失败，清单已经没了、库里这款消失，源 IMG 还在导入文件夹
static void mf_aside(const char *root, int back) {
    static const char *const nm[] = {"manifest.json", ".applied"};
    for (int i = 0; i < 2; i++) { char a[2400], b[2500]; snprintf(a, sizeof a, "%s/%s", root, nm[i]); snprintf(b, sizeof b, "%s.s2a-prev", a);
        if (back == 1) rename(b, a); else if (back == 2) unlink(b); else { unlink(b); rename(a, b); } }
}
// 资源包里一次换件的回滚记录（审计 P1-169 兄弟站点：写 index.tsv / 清单失败时，已经换上的 game.img、派生件、目录表要退回去）：
//   tx_swap 把 dst 原有的那份改名成 dst.s2a-prev 再把 src 改名进来，记下；失败时 tx_undo 倒序换回（新进来的删掉 / 挪回原处），成功时 tx_done 删掉各份 .s2a-prev。
//   全是同一目录树里的 rename，不拷贝
typedef struct { char dst[2400], back[2400]; int had_prev; } tx_ent;
// 记录按需扩容（D-455Z / D-MACF：原先固定 64 格，着色器更新包的派生件超过 63 个就报 E2BIG 整份拒收——
//   派生件个数没有上限，物理上换得下去，不该被一张定长表拦住）；扩不了（内存不够）才失败、照常回滚
typedef struct { tx_ent *e; int n, cap; } tx_t;
static void tx_free(tx_t *t) { if (t) { free(t->e); free(t); } }
static int tx_swap(tx_t *t, const char *src, const char *dst, const char *undo_to) {   // undo_to：失败时把新件挪回哪里（NULL = 删掉）
    if (t->n == t->cap) { int cap = t->cap ? t->cap * 2 : 64; tx_ent *e = realloc(t->e, (size_t)cap * sizeof *e); if (!e) { errno = ENOMEM; return -1; } t->e = e; t->cap = cap; }
    tx_ent *x = &t->e[t->n]; snprintf(x->dst, sizeof x->dst, "%s", dst); snprintf(x->back, sizeof x->back, "%s", undo_to ? undo_to : "");
    char prev[2500]; snprintf(prev, sizeof prev, "%s.s2a-prev", dst); unlink(prev);
    struct stat st; x->had_prev = !lstat(dst, &st) && !S_ISDIR(st.st_mode) && !rename(dst, prev);   // 位置被目录占着：不挪它，下面改名照旧失败（同原来）
    if (rename(src, dst)) { int e = errno; if (x->had_prev) rename(prev, dst); errno = e; return -1; }
    t->n++; return 0;
}
static void tx_undo(tx_t *t) {
    for (int i = t->n - 1; i >= 0; i--) { tx_ent *x = &t->e[i]; char prev[2500]; snprintf(prev, sizeof prev, "%s.s2a-prev", x->dst);
        if (x->back[0]) rename(x->dst, x->back); else unlink(x->dst);
        if (x->had_prev) rename(prev, x->dst); }
    t->n = 0;
}
static void tx_done(tx_t *t) { for (int i = 0; i < t->n; i++) { char prev[2500]; snprintf(prev, sizeof prev, "%s.s2a-prev", t->e[i].dst); unlink(prev); } t->n = 0; }
static int img_update(const char *proc_docs, const char *path, const uint8_t *h, uint64_t total) {
    char gid[17], pid[65]; memcpy(gid, h + 16, 16); gid[16] = 0; memcpy(pid, h + 32, 64); pid[64] = 0;
    const char *fn = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    char root[1400], base[1500], gdocs[1200]; snprintf(gdocs, sizeof gdocs, "%s/games/%s", proc_docs, gid); s2a_pack_root(root, sizeof root, gdocs);
    if (pack_in_bundle(root, "认领着色器更新包")) { snprintf(g_img_err, sizeof g_img_err, "%s：这款的资源包在 App 包内，不能更新", fn); return -4; }
    snprintf(base, sizeof base, "%s/game.img", root);
    uint8_t bh[136]; int bfd = open(base, O_RDONLY); int ok = bfd >= 0 && pread(bfd, bh, sizeof bh, 0) == (ssize_t)sizeof bh && !memcmp(bh, "S2AIMG01", 8); if (bfd >= 0) close(bfd);
    if (!ok) { snprintf(g_img_err, sizeof g_img_err, "%s 是着色器更新包，手机上还没有这款游戏（%s）的本体：先放导出文件夹里这款的完整 .img", fn, gid); return -13; }
    // 配的本体不是手机上这一版（维护者 11-17：玩家自己判断不了配不配套，替他判——不套用、不报错，游戏照常玩，说一句已跳过）：
    //   返回 2 = 跳过（不算认领、不算失败），文件原样留在导入文件夹；库上提示一次（s2a_img_scan_skipped，app.m img_scan_report）
    if (memcmp(bh + 32, h + 32, 64)) { char bp[65]; memcpy(bp, bh + 32, 64); bp[64] = 0;
        LOG("资源导入：%s 配的本体（资源包号 %.12s）不是手机上这一版（%.12s），和当前游戏版本不配套，已跳过、不套用", fn, pid, bp);
        snprintf(g_img_skipped, sizeof g_img_skipped, "%s", fn); return 2; }
    // 本体刚认领、游戏库还没解开（.applied 不是这份清单）：先不套。解开时要按目录表整理资源包目录，这边同时往 shaders/ 写派生件会被删掉、
    //   报「写 … 失败：No such file or directory」（诊断#fe33c2 D-33EH：完整 .img 与更新包一起放，更新包在本体解开途中被套）。返回 1 = 原样留着，下次扫描（回前台 / 重新检查）再套
    if (img_needs_apply(root)) { LOG("资源导入：%s 等本体解开后再套（本体还没解开）", fn); return 1; }
    int ufd = open(path, O_RDONLY); if (ufd < 0) { snprintf(g_img_err, sizeof g_img_err, "读 %s 失败：%s", fn, strerror(errno)); return -4; }
    uint64_t to, tl; memcpy(&to, h + 112, 8); memcpy(&tl, h + 120, 8); upd_ent *ue; size_t un;
    if (upd_read_toc(ufd, to, tl, &ue, &un)) { close(ufd); snprintf(g_img_err, sizeof g_img_err, "%s 目录表坏了", fn); return -10; }
    (void)total;
    // 审计 P1-169：派生件先全部写成临时名（.upd.tmp），一个都写不成就删掉临时件、什么都不动（清单、本体目录表都在，这款照旧能玩）；
    //   都写成了才作废清单（留退路）、整理本体目录表、换名、写 index.tsv 和清单。原先先删清单再改本体目录表，派生件写到一半失败，这款从库里消失
    int rc = 0; uint64_t bytes = 0;
    for (size_t i = 0; i < un && !rc; i++) {
        if (!upd_is_extra(ue[i].rel) || strstr(ue[i].rel, "..") || ue[i].rel[0] == '/') continue;   // 只认派生件路径
        char dst[2400], tmp[2500]; snprintf(dst, sizeof dst, "%s/%s", root, ue[i].rel); snprintf(tmp, sizeof tmp, "%s.upd.tmp", dst);
        if (mkdirs(dst)) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "建 %s 的目录失败", ue[i].rel); break; }
        int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (o < 0) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 %s 失败：%s", ue[i].rel, strerror(errno)); break; }
        uint8_t buf[1 << 16]; uint64_t left = ue[i].len, at = ue[i].off;
        while (left && !rc) { size_t want = left < sizeof buf ? (size_t)left : sizeof buf; ssize_t r = pread(ufd, buf, want, (off_t)at);
            if (r <= 0 || write(o, buf, (size_t)r) != r) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 %s 失败：%s", ue[i].rel, strerror(errno ? errno : EIO)); break; }
            left -= (uint64_t)r; at += (uint64_t)r; }
        // game.imgmap 偏移表头里的整长要等整理完本体才知道：换名前再补（见下）
        if (close(o) || rc) { if (!rc) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 %s 失败：%s", ue[i].rel, strerror(errno)); } unlink(tmp); break; }
        bytes += ue[i].len;
    }
    close(ufd);
    // 临时件的目标位置得能换名进去（被目录占着、换不进去时也要在动清单之前就失败）
    for (size_t i = 0; i < un && !rc; i++) { if (!upd_is_extra(ue[i].rel) || strstr(ue[i].rel, "..") || ue[i].rel[0] == '/') continue;
        char dst[2400]; struct stat ds; snprintf(dst, sizeof dst, "%s/%s", root, ue[i].rel);
        if (!lstat(dst, &ds) && S_ISDIR(ds.st_mode)) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 %s 失败：那里是一个目录", ue[i].rel); } }
    int64_t nt = 0; tx_t *tx = calloc(1, sizeof *tx); if (!tx && !rc) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "内存不够"); }
    // 本体目录表在副本上整理（APFS 克隆：不占空间、瞬间完成），换名进去时旧本体留作退路；克隆不了（非 APFS）才退回原地整理（失败时本体退不回去）
    char nb[1600]; snprintf(nb, sizeof nb, "%s.upd.tmp", base); unlink(nb);
    int cloned = 0;
#if defined(__APPLE__)
    if (!rc) cloned = !clonefile(base, nb, 0);
#endif
    if (!rc) { mf_aside(root, 0);   // 先作废（留退路）：写到一半断了不会判成就绪
        nt = upd_strip_base(cloned ? nb : base);
        if (nt < 0) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "整理本体 game.img 的目录表失败：%s", strerror(errno)); }
        else if (cloned && tx_swap(tx, nb, base, NULL)) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "换上整理过的本体失败：%s", strerror(errno)); } }
    if (cloned && rc) unlink(nb);
    for (size_t i = 0; i < un; i++) { if (!upd_is_extra(ue[i].rel) || strstr(ue[i].rel, "..") || ue[i].rel[0] == '/') continue;
        char dst[2400], tmp[2500]; snprintf(dst, sizeof dst, "%s/%s", root, ue[i].rel); snprintf(tmp, sizeof tmp, "%s.upd.tmp", dst);
        if (rc) { unlink(tmp); continue; }   // 前面失败：临时件全删
        if (!strcmp(ue[i].rel, "game.imgmap") && ue[i].len >= 16) { int o = open(tmp, O_WRONLY); int64_t t64 = nt;   // 偏移表头里记的是整个 game.img 的长度（RomFsIndex.OpenImg 核它）
            if (o < 0 || pwrite(o, &t64, 8, 8) != 8) rc = -4; if (o >= 0) close(o); }
        if (!rc && tx_swap(tx, tmp, dst, NULL)) rc = -4;
        if (rc) { snprintf(g_img_err, sizeof g_img_err, "写 %s 失败：%s", ue[i].rel, strerror(errno)); unlink(tmp); } }
    if (!rc) {   // index.tsv：game.img 一行（新整长）+ 更新包里的派生件；原有的其余行照留，旧派生件行去掉
        char ip[2400], tp[2500]; snprintf(ip, sizeof ip, "%s/index.tsv", root); snprintf(tp, sizeof tp, "%s.tmp", ip);
        FILE *o = fopen(tp, "w"), *in = fopen(ip, "r");
        if (!o) { if (in) fclose(in); rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 index.tsv 失败：%s", strerror(errno)); }
        else {
            fprintf(o, "game.img\t%lld\t%s\n", (long long)nt, pid);
            if (in) { char line[4600]; while (fgets(line, sizeof line, in)) { char rel[4600]; snprintf(rel, sizeof rel, "%s", line); rel[strcspn(rel, "\t\n")] = 0;
                if (strcmp(rel, "game.img") && !upd_is_extra(rel)) fputs(line, o); } fclose(in); }
            for (size_t i = 0; i < un; i++) if (upd_is_extra(ue[i].rel)) fprintf(o, "%s\t%llu\t-\n", ue[i].rel, (unsigned long long)ue[i].len);
            if (fclose(o) || tx_swap(tx, tp, ip, NULL)) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 index.tsv 失败：%s", strerror(errno)); unlink(tp); }
        }
    }
    upd_free(ue, un);
    if (!rc) {
        char mf[600], p[2400], tp[2500]; snprintf(mf, sizeof mf, "{\"version\": 3, \"format\": \"img\", \"imgFile\": \"game.img\", \"index_sha256\": \"%.40s-u%llx\"}\n", pid, (unsigned long long)bytes);
        snprintf(p, sizeof p, "%s/manifest.json", root); snprintf(tp, sizeof tp, "%s.tmp", p);
        FILE *m = fopen(tp, "w"); if (!m || fputs(mf, m) < 0 || fclose(m) || rename(tp, p)) { rc = -4; snprintf(g_img_err, sizeof g_img_err, "写 manifest.json 失败：%s", strerror(errno)); unlink(tp); }
        else mark_imported(root, mf);
    }
    if (rc) {   // 写到一半失败：已经换上的本体 / 派生件 / 目录表全部退回，清单改回原名——这款照旧是套更新包之前的样子、照旧能玩
        if (tx) { tx_undo(tx); tx_free(tx); }
        if (!cloned && nt > 0) LOG("【红】资源导入：%s 套到一半失败，本体是原地整理的（不是 APFS、克隆不了），目录表退不回去：下次解包按清单核对会重取", fn);
        mf_aside(root, 1); return rc; }
    tx_done(tx); tx_free(tx);
    mf_aside(root, 2);
    unlink(path);   // 已套上：导入文件夹里的更新包删掉（派生件已在资源包里）
    LOG("资源导入：着色器更新包 %s 套到 %s（%llu MB 派生件，本体没动）", fn, gid, (unsigned long long)(bytes >> 20));
    return 0;
}
// pass 0 只认完整包、pass 1 只认着色器更新包（诊断#0787f1 D-YY32：完整 .img 与更新包一起放，同一次扫描里更新包排在前面，
//   先报「手机上还没有本体」，紧接着本体才挪好——玩家看到假红；本体都先挪好再看更新包，与文件名 / 目录顺序无关）
static int img_place(const char *proc_docs, const char *path, int pass) {   // 0 = 挪好了；1 = 不是 IMG / 还在拷 / 不归这一遍（不动）；负数 = 失败（原样留着）
    int fd = open(path, O_RDONLY); if (fd < 0) return 1;
    uint8_t h[136]; struct stat st; int ok = pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h, "S2AIMG01", 8) && !fstat(fd, &st); close(fd);
    if (!ok) return 1;
    uint64_t total; memcpy(&total, h + 128, 8); if (total != (uint64_t)st.st_size) return 1;   // 还在拷
    uint32_t fmt; memcpy(&fmt, h + 8, 4);
    // 长度对上不等于拷完（D-4VAX 复现：拷贝方先把目标撑到全长、再从头往后填，App 扫到时头在、尾巴的目录表还是 0，原先照样认领挪走，
    //   拷贝方经原句柄接着写进挪走后的文件，期间再解包 / 套更新包读到的是半截目录表）：目录表在文件末尾、最后才写到——
    //   读得出、自检过（img_toc_file）、且列着程序描述 exefs/main.npdm 才算拷完；更新包要目录表读得出且非空。不然当「还在拷」，原样留着下次再看
    if (fmt == 1) { img_ent *te; size_t tn; int has = 0;
        if (img_toc_file(path, &te, &tn)) return 1;
        for (size_t i = 0; i < tn && !has; i++) has = !strcmp(te[i].rel, "exefs/main.npdm");
        img_free(te, tn); if (!has) return 1; }
    if (fmt == 2) { int ufd = open(path, O_RDONLY); upd_ent *ue = NULL; size_t un = 0; uint64_t uto, utl; memcpy(&uto, h + 112, 8); memcpy(&utl, h + 120, 8);
        int bad = ufd < 0 || upd_read_toc(ufd, uto, utl, &ue, &un) || !un; if (ufd >= 0) close(ufd); if (ue) upd_free(ue, un); if (bad) return 1; }
    if ((fmt == 2) != (pass == 1)) return 1;
    if (fmt == 2) return img_update(proc_docs, path, h, total);   // 着色器更新包
    if (fmt != 1) LOG("导入：%s 是格式号 %u 的资源包，这个 S2Arm 只认 1、2，按格式 1 照常导入；读不出来再报（维护者 11-17：校验不过不整个拒绝）", path, fmt);
    char gid[17], pid[65]; memcpy(gid, h + 16, 16); gid[16] = 0; memcpy(pid, h + 32, 64); pid[64] = 0;
    for (int i = 0; i < 16; i++) if (!((gid[i] >= '0' && gid[i] <= '9') || (gid[i] >= 'A' && gid[i] <= 'F'))) { snprintf(g_img_err, sizeof g_img_err, "%s 头里的游戏号不对", path); return -10; }
    if (!pid[0]) snprintf(pid, sizeof pid, "img-%llx", (unsigned long long)total);   // 没写资源包号的（老工具）：按长度给一个，至少换了文件能触发重新应用
    char root[1400], p[2400], old[2400], gdocs[1200]; snprintf(gdocs, sizeof gdocs, "%s/games/%s", proc_docs, gid); s2a_pack_root(root, sizeof root, gdocs);
    if (pack_in_bundle(root, "认领 IMG")) { snprintf(g_img_err, sizeof g_img_err, "%s：这款的资源包在 App 包内，不能换", path); return -4; }
    snprintf(p, sizeof p, "%s/game.img", root); if (mkdirs(p)) { snprintf(g_img_err, sizeof g_img_err, "建 %s 失败：%s", root, g_untar_err); return -4; }
    (void)old; tx_t *tx = calloc(1, sizeof *tx); if (!tx) { snprintf(g_img_err, sizeof g_img_err, "内存不够"); return -4; }
    mf_aside(root, 0);   // 先作废（留退路）：挪到一半断了不会判成就绪
    // 旧 game.img 改名留作退路（审计 P1-169 兄弟站点）：后面 index.tsv / 清单写失败时新包挪回导入处、旧包与旧目录表换回，这款照旧能玩
    if (tx_swap(tx, path, p, path)) { int e = errno; tx_free(tx); mf_aside(root, 1); snprintf(g_img_err, sizeof g_img_err, "把 %s 挪进资源包失败：%s", path, strerror(e)); return -4; }
#define IMG_PLACE_FAIL() do { int e_ = errno; tx_undo(tx); tx_free(tx); mf_aside(root, 1); errno = e_; return -4; } while (0)
    // index.tsv：game.img 一行 + 原有 index.tsv 里的直连库 / 覆盖层行（着色器库与覆盖层是 App 管的缓存、不进 IMG；电脑推过的那部分不丢）
    char ip[2400], tp[2500]; snprintf(ip, sizeof ip, "%s/index.tsv", root); snprintf(tp, sizeof tp, "%s.tmp", ip);
    FILE *o = fopen(tp, "w"), *in = fopen(ip, "r"); if (!o) { if (in) fclose(in); snprintf(g_img_err, sizeof g_img_err, "写 index.tsv 失败：%s", strerror(errno)); IMG_PLACE_FAIL(); }
    fprintf(o, "game.img\t%llu\t-\n", (unsigned long long)total);
    int shaders = 0;   // 着色器库从哪来：原有 index.tsv（电脑推过）或 IMG 目录表（导出文件夹里的「<游戏名>.img」带着，pack/img.py export）
    if (in) { char line[4600]; while (fgets(line, sizeof line, in)) if (!strncmp(line, "shaders/", 8) || !strncmp(line, "romfs_overlay/", 14)) { fputs(line, o); shaders |= !strncmp(line, "shaders/", 8); } fclose(in); }
    if (!shaders) { img_ent *te; size_t tn; if (!img_toc(root, &te, &tn)) { for (size_t i = 0; i < tn && !shaders; i++) shaders = !strncmp(te[i].rel, "shaders/", 8); img_free(te, tn); } }
    // 两处都没有：多半是把资源包文件夹里的 game.img 直接放了进来（它按设计不带 shaders/），起局时着色器全靠现编（11-13 起不再收局；诊断 D-9864 / D-F6W7）
    if (!shaders) { LOG("【黄】资源导入：%s 里没有着色器库（shaders/）：照样能玩，着色器首次遇到时现编、前几分钟会卡；要不卡就把电脑「S2Arm 游戏包」文件夹里这款的 .s2arm 一起放进来（它带着着色器库）",
                        strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
                    snprintf(g_img_noshaders, sizeof g_img_noshaders, "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path); }
    if (fclose(o) || tx_swap(tx, tp, ip, NULL)) { snprintf(g_img_err, sizeof g_img_err, "写 index.tsv 失败：%s", strerror(errno)); unlink(tp); IMG_PLACE_FAIL(); }
    char mf[600]; snprintf(mf, sizeof mf, "{\"version\": 3, \"format\": \"img\", \"imgFile\": \"game.img\", \"index_sha256\": \"%s\"}\n", pid);
    snprintf(p, sizeof p, "%s/manifest.json", root); snprintf(tp, sizeof tp, "%s.tmp", p);
    FILE *m = fopen(tp, "w"); if (!m || fputs(mf, m) < 0 || fclose(m) || rename(tp, p)) { snprintf(g_img_err, sizeof g_img_err, "写 manifest.json 失败：%s", strerror(errno)); unlink(tp); IMG_PLACE_FAIL(); }
    mark_imported(root, mf);
#undef IMG_PLACE_FAIL
    tx_done(tx); tx_free(tx);
    mf_aside(root, 2);
    LOG("资源导入：%s → %s/game.img（%llu MB），等游戏库解开", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, root, (unsigned long long)(total >> 20));
    return 0;
}
// ---- 小游戏包 .s2arm 导入（0.6.0 通用壳，手机侧；电脑端 pack/jobs/install.py export_s2arm 出）----
//   文件 = 一个带内容校验的 zstd 帧，里面是 ustar（只有普通文件）：第一个条目 s2arm.json（format "s2arm"、schema 1、title_id、image_sha256…），
//   program/（未签名程序包：dylib、Games/<号>/、catalogue.json）、pack/（资源包里除 game.img 以外的派生件）。game.img 不在里面，
//   玩家另放进「放这里导入」，照常由 img_place 认领（它保留资源包里原有 index.tsv 的着色器库行）。
//   落盘同电脑推程序包的写入规则：程序写进自己的版本目录 program/v-<image_sha256 前 16 位>-s2arm/，最后临时名改名换上 catalogue.json（补 dir）；
//   签名不在这里做：清单 team 为空，起局时 ios/program_resign.m 用手机上的身份签这一款。派生件逐个临时名改名写进 资源包/。
//   返回 0 = 导入好（gid 写这一款的游戏号）；1 = 文件还在拷（zstd 帧没到尾，原样留着下次再看）；负数 = 失败，s2a_s2arm_import_code / _failure 给码与原因
static char g_s2arm_err[1600], g_s2arm_code[48];
const char *s2a_s2arm_import_failure(void) { return g_s2arm_err; }
const char *s2a_s2arm_import_code(void) { return g_s2arm_code; }
static int s2arm_fail(int rc, const char *code, const char *fmt, ...) {
    snprintf(g_s2arm_code, sizeof g_s2arm_code, "%s", code);
    va_list ap; va_start(ap, fmt); vsnprintf(g_s2arm_err, sizeof g_s2arm_err, fmt, ap); va_end(ap);
    LOG("【红】.s2arm 导入（%s）：%s", g_s2arm_code, g_s2arm_err);   // 每条失败都进日志（11-17 真机：解压 / 建临时文件阶段失败原先只设码不打日志，看不出是没扫到还是失败）
    return rc;
}
static int s2arm_json_str(const char *j, const char *key, char *out, size_t n) {   // 电脑端 json.dumps 写的平铺字符串字段（不处理嵌套）
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j, pat); if (!p) return -1;
    p += strlen(pat); while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '"') return -1;
    size_t k = 0; for (p++; *p && *p != '"' && k + 1 < n; p++) { if (*p == '\\' && p[1]) p++; out[k++] = *p; }
    out[k] = 0; return *p == '"' ? 0 : -1;
}
static int s2arm_safe(const char *rel) {   // 条目路径不出目标目录：非空、不以 / 开头、没有 .. 段、没有反斜杠
    if (!*rel || *rel == '/' || strchr(rel, '\\')) return 0;
    for (const char *s = rel; *s; ) { const char *e = strchr(s, '/'); size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!l || (l == 2 && s[0] == '.' && s[1] == '.') || (l == 1 && s[0] == '.')) return 0; s += l + (e ? 1 : 0); if (!e) break; }
    return 1;
}
static int s2arm_rm_one(const char *f, const struct stat *st, int flag, struct FTW *w) { (void)st; (void)w; return flag == FTW_DP ? rmdir(f) : unlink(f); }
static int s2arm_copy_out(int fd, uint64_t off, uint64_t size, const char *dst) {   // tar 里 [off, off+size) 写到 dst（临时名再改名）
    char tmp[2600]; snprintf(tmp, sizeof tmp, "%s.%d.part", dst, (int)getpid());
    char d[2600]; snprintf(d, sizeof d, "%s", dst); if (mkdirs(d)) return -1;
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (o < 0) return -1;
    static char buf[1 << 20]; uint64_t done = 0; int ok = 1;
    while (ok && done < size) { size_t want = size - done < sizeof buf ? (size_t)(size - done) : sizeof buf;
        ssize_t r = pread(fd, buf, want, (off_t)(off + done)); if (r <= 0) { ok = 0; errno = r < 0 ? errno : EIO; break; }
        for (ssize_t w = 0; w < r; ) { ssize_t k = write(o, buf + w, (size_t)(r - w)); if (k <= 0) { ok = 0; break; } w += k; }
        done += (uint64_t)r; }
    int e = errno; if (close(o)) { ok = 0; e = errno; }
    if (!ok || rename(tmp, dst)) { if (ok) e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}
int s2a_s2arm_import(const char *proc_docs, const char *src, char *gid_out, size_t gid_n) {
    g_s2arm_err[0] = g_s2arm_code[0] = 0; if (gid_out && gid_n) gid_out[0] = 0;
    char base[1200]; if (s2a_pack_base(base, sizeof base, proc_docs) <= 0) return s2arm_fail(-3, "ios_install_dir_unwritable", "文档根 %s 不是 …/Documents，找不到资源位置", proc_docs);
    char tarp[1400]; snprintf(tarp, sizeof tarp, "%s/.s2arm-import.%d.tar", base, (int)getpid());
    { char t[1400]; snprintf(t, sizeof t, "%s", tarp); mkdirs(t); }
    // ① 解压到临时 tar（流式，1 MB 缓冲）：帧没到尾 = 还在拷，原样留着
    int in = open(src, O_RDONLY); if (in < 0) return s2arm_fail(-1, "ios_install_pack_truncated", "打不开 %s：%s", src, strerror(errno));
    int out = open(tarp, O_RDWR | O_CREAT | O_TRUNC, 0600); if (out < 0) { int e = errno; close(in); return s2arm_fail(-4, e == ENOSPC ? "ios_install_no_space" : "ios_install_dir_unwritable", "建临时文件 %s 失败：%s", tarp, strerror(e)); }
    ZSTD_DStream *z = ZSTD_createDStream(); ZSTD_initDStream(z);
    static char ib[1 << 20], ob[1 << 20]; size_t last = 1; int rc = 0; ssize_t r;
    while (!rc && (r = read(in, ib, sizeof ib)) > 0) {
        ZSTD_inBuffer zi = {ib, (size_t)r, 0};
        while (zi.pos < zi.size) { ZSTD_outBuffer zo = {ob, sizeof ob, 0}; last = ZSTD_decompressStream(z, &zo, &zi);
            if (ZSTD_isError(last)) { rc = s2arm_fail(-1, "ios_install_pack_truncated", "%s 不是完整的 .s2arm（解压：%s）", src, ZSTD_getErrorName(last)); break; }
            for (size_t w = 0; w < zo.pos; ) { ssize_t k = write(out, ob + w, zo.pos - w); if (k <= 0) { int e = errno; rc = s2arm_fail(-4, e == ENOSPC ? "ios_install_no_space" : "ios_install_dir_unwritable", "写临时文件失败：%s", strerror(e)); break; } w += (size_t)k; }
            if (rc) break; }
    }
    ZSTD_freeDStream(z); close(in);
    if (!rc && last != 0) { rc = 1; LOG(".s2arm 导入：%s 的 zstd 帧还没读到尾，当作还在拷，原样留着，下次扫再看", src); }   // 帧没到尾：还在拷（或拷断了）
    if (rc) { close(out); unlink(tarp); return rc; }
    // ② 读 tar：第一个条目必须是 s2arm.json
    char gid[17] = "", sha[80] = "", root[1400], prog[1400], vdir[64], fmt[32] = "", *cat = NULL; size_t catn = 0; unsigned nprog = 0, npack = 0; uint64_t off = 0;
    for (int first = 1;; first = 0) {
        unsigned char h[512]; if (pread(out, h, 512, (off_t)off) != 512) { rc = s2arm_fail(-1, "ios_install_pack_truncated", ".s2arm 里的 tar 截短了（第 %llu 字节）", (unsigned long long)off); break; }
        int zero = 1; for (int i = 0; i < 512 && zero; i++) zero = !h[i]; if (zero) break;
        char name[260]; char pre[156], nm[101]; memcpy(pre, h + 345, 155); pre[155] = 0; memcpy(nm, h, 100); nm[100] = 0;
        snprintf(name, sizeof name, "%s%s%s", pre, pre[0] ? "/" : "", nm);
        char szs[13]; memcpy(szs, h + 124, 12); szs[12] = 0; uint64_t size = strtoull(szs, NULL, 8); char type = (char)h[156];
        uint64_t data = off + 512; off = data + (size + 511) / 512 * 512;
        if (first) {
            if (strcmp(name, "s2arm.json") || size > 65536) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "不是 S2Arm 小游戏包：第一个条目是 %s", name); break; }
            char j[65537]; if (pread(out, j, (size_t)size, (off_t)data) != (ssize_t)size) { rc = s2arm_fail(-1, "ios_install_pack_truncated", "s2arm.json 读不全"); break; } j[size] = 0;
            s2arm_json_str(j, "format", fmt, sizeof fmt); s2arm_json_str(j, "title_id", gid, sizeof gid); s2arm_json_str(j, "image_sha256", sha, sizeof sha);
            if (strcmp(fmt, "s2arm")) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "s2arm.json 的 format 是「%s」，不是 s2arm", fmt); break; }
            for (int i = 0; i < 16; i++) if (gid[i] >= 'a' && gid[i] <= 'f') gid[i] -= 32;
            int ok = strlen(gid) == 16; for (int i = 0; i < 16 && ok; i++) ok = (gid[i] >= '0' && gid[i] <= '9') || (gid[i] >= 'A' && gid[i] <= 'F');
            if (!ok) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "s2arm.json 的游戏号「%s」不对", gid); break; }
            if (strstr(j, "\"schema\": 1") == NULL && strstr(j, "\"schema\":1") == NULL) LOG("【黄】.s2arm 导入：%s 的 schema 不是 1，按 1 的布局照常导入（维护者 11-17：校验不过不整个拒绝）", gid);
            snprintf(vdir, sizeof vdir, "v-%.16s-s2arm", strlen(sha) >= 16 ? sha : "0000000000000000");
            snprintf(root, sizeof root, "%s/games/%s/资源包", base, gid); snprintf(prog, sizeof prog, "%s/games/%s/program", base, gid);
            char vd[1500]; snprintf(vd, sizeof vd, "%s/%s", prog, vdir); struct stat st;
            if (!lstat(vd, &st)) nftw(vd, s2arm_rm_one, 16, FTW_DEPTH | FTW_PHYS);   // 上次导到一半的同一版：清掉重写
            continue;
        }
        if (type != '0' && type != 0) continue;   // 目录条目等：文件写入时自己建目录
        const char *rel; char dst[2600];
        if (!strcmp(name, "program/catalogue.json")) {
            if (size > (1u << 20)) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "程序包清单太大（%llu 字节）", (unsigned long long)size); break; }
            free(cat); cat = malloc((size_t)size + 1); catn = (size_t)size;
            if (!cat || pread(out, cat, catn, (off_t)data) != (ssize_t)catn) { rc = s2arm_fail(-1, "ios_install_pack_truncated", "程序包清单读不全"); break; } cat[catn] = 0; continue;
        }
        if (!strncmp(name, "program/", 8)) { rel = name + 8; if (!s2arm_safe(rel)) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "条目路径不对：%s", name); break; } snprintf(dst, sizeof dst, "%s/%s/%s", prog, vdir, rel); nprog++; }
        else if (!strncmp(name, "pack/", 5)) { rel = name + 5; if (!s2arm_safe(rel) || !strcmp(rel, "game.img")) { rc = s2arm_fail(-2, "ios_install_manifest_bad", "条目路径不对：%s", name); break; } snprintf(dst, sizeof dst, "%s/%s", root, rel); npack++; }
        else { LOG("【黄】.s2arm 导入：不认识的条目 %s，跳过", name); continue; }
        if (s2arm_copy_out(out, data, size, dst)) { int e = errno; rc = s2arm_fail(-4, e == ENOSPC || e == EDQUOT ? "ios_install_no_space" : "ios_install_dir_unwritable", "写 %s 失败：%s", dst, strerror(e)); break; }
    }
    close(out); unlink(tarp);
    if (!rc && !cat) rc = s2arm_fail(-2, "ios_install_manifest_bad", "%s 里没有程序包清单 program/catalogue.json", src);
    if (!rc) {   // ③ 最后换上程序包清单（补 dir 指向这一版）：之前任何一步失败，这一款都还是导入前的样子
        char cp[1500], tp[1600]; snprintf(cp, sizeof cp, "%s/catalogue.json", prog); snprintf(tp, sizeof tp, "%s.%d.tmp", cp, (int)getpid());
        const char *b = strchr(cat, '{'); FILE *f = b ? fopen(tp, "w") : NULL;
        int ok = f && fprintf(f, "{\"dir\": \"%s\",%s", vdir, b + 1) > 0; if (f && fclose(f)) ok = 0;
        if (!ok || rename(tp, cp)) { int e = errno; unlink(tp); rc = s2arm_fail(-4, e == ENOSPC ? "ios_install_no_space" : "ios_install_dir_unwritable", "写程序包清单 %s 失败：%s", cp, strerror(e)); }
    }
    free(cat);
    if (rc) return rc;   // 原因已在 s2arm_fail 记过
    if (gid_out && gid_n) snprintf(gid_out, gid_n, "%s", gid);
    LOG(".s2arm 导入：%s → 程序 %s（%u 个文件）、资源包派生件 %u 个；game.img 照常放「放这里导入」认领", gid, vdir, nprog, npack);
    return 0;
}
// 导入文件夹里认领 / 导入失败过的文件（0.6.0 自签走查 B1）：原先失败的文件原样留着、每次扫描（回前台、文件夹有写入、「重新检查」）都整份重新解压 / 读一遍。
//   按「路径 + 大小 + 修改时间」记住，没变就跳过；玩家换了文件、或文件还在拷而变大，下次照常再试。进程内表（重开 App 会再试一次、再报一次）。持 s2a_img_scan 的锁用
static struct { char path[600]; off_t size; long long mtime; } g_scan_failed[32]; static int g_scan_failed_n;
static long long st_mtime_ns(const struct stat *st) {   // 苹果叫 st_mtimespec，安卓 / Linux 叫 st_mtim（本文件安卓也编）
#ifdef __APPLE__
    return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
    return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}
static int scan_failed_same(const char *p, const struct stat *st) {
    for (int i = 0; i < g_scan_failed_n; i++)
        if (!strcmp(g_scan_failed[i].path, p)) return g_scan_failed[i].size == st->st_size && g_scan_failed[i].mtime == st_mtime_ns(st);
    return 0;
}
static void scan_failed_note(const char *p, const struct stat *st) {
    int i = 0; while (i < g_scan_failed_n && strcmp(g_scan_failed[i].path, p)) i++;
    if (i == g_scan_failed_n) { if (g_scan_failed_n < 32) g_scan_failed_n++; else i = (int)(st->st_ino % 32); }
    snprintf(g_scan_failed[i].path, sizeof g_scan_failed[i].path, "%s", p); g_scan_failed[i].size = st->st_size;
    g_scan_failed[i].mtime = st_mtime_ns(st);
}
static int img_scan_dir(const char *proc_docs, const char *dir, int depth, int pass, int *placed) {
    DIR *d = opendir(dir); if (!d) return 0;
    char *names[512]; int nn = 0; struct dirent *e;
    while ((e = readdir(d)) && nn < 512) { if (e->d_name[0] == '.') continue; if (depth == 0 && !strcmp(e->d_name, "games")) continue; names[nn++] = strdup(e->d_name); }
    closedir(d);
    // 同一个文件夹里先导 .s2arm、再认领 IMG：.s2arm 带来的资源包派生件里有 index.tsv 的着色器库行，IMG 认领时照留（img_place）。readdir 的次序不定，
    //   IMG 先认领时 index.tsv 还没到，报「没有着色器库、首次遇到时现编」，可接着导入的 .s2arm 已经带着着色器（11-18 两台 iPhone 实机：起局着色器表 11 行、现编 0）
    { char *ord[512]; int m = 0; for (int k = 0; k < 2; k++) for (int j = 0; j < nn; j++) if ((ends_with(names[j], ".s2arm") != 0) == (k == 0)) ord[m++] = names[j];
      memcpy(names, ord, (size_t)nn * sizeof *names); }   // 稳定分区：.s2arm 在前，其余保持原次序
    for (int i = 0; i < nn; i++) {
        char p[2400]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (!lstat(p, &st)) {
            if (S_ISDIR(st.st_mode) && depth < 2) img_scan_dir(proc_docs, p, depth + 1, pass, placed);
            else if (S_ISREG(st.st_mode) && scan_failed_same(p, &st)) { /* 上次失败、文件没变：不再整份重解 */ }
            else if (S_ISREG(st.st_mode) && pass == 0 && ends_with(names[i], ".s2arm")) { char g[17]; int r = s2a_s2arm_import(proc_docs, p, g, sizeof g);
                // 导好就删（同 IMG 认领后挪走）；失败原样留着，码与原因都交给库界面（原先只给码、原因空 → 库上不弹，失败没有一点声音）
                if (!r) { unlink(p); (*placed)++; }
                else if (r < 0) { snprintf(g_img_code, sizeof g_img_code, "%s", g_s2arm_code); snprintf(g_img_err, sizeof g_img_err, "%s：%s", names[i], g_s2arm_err[0] ? g_s2arm_err : g_s2arm_code);
                    if (!strstr(g_img_code, "space")) scan_failed_note(p, &st);   // 空间不够：腾出来后同一个文件要能再导，不记
                    LOG("【红】资源导入（%s）：%s", g_img_code, g_img_err); } }
            else if (S_ISREG(st.st_mode) && st.st_size >= 4096) { int r = img_place(proc_docs, p, pass); if (!r) (*placed)++; else if (r < 0) { int e = errno; if (r != -13 && e != ENOSPC && e != EDQUOT) scan_failed_note(p, &st);   // 更新包等本体（-13）、空间不够：条件变了同一个文件要再试，不记
                snprintf(g_img_code, sizeof g_img_code, "%s", r == -13 ? "ios_import_update_no_base" : r == -14 ? "ios_import_update_mismatch" : r == -11 ? "ios_install_img_format" : r == -4 && (e == ENOSPC || e == EDQUOT) ? "ios_install_no_space" : "ios_import_img_failed");
                LOG("【红】资源导入（%s）：%s", g_img_code, g_img_err); } }
        }
        free(names[i]);
    }
    return 0;
}
// 认领只扫 Documents 下的导入文件夹（v0.3.0 起，原先扫整个 Documents）：名字按界面语言建（app.m），两种都认，切了语言照样认
const char *const s2a_img_import_dirs[2] = {"放这里导入", "Import Here"};
int s2a_img_scan(const char *proc_docs) {   // 回前台 / 「重新检查」可能同时调：一把锁串起来，免得两边抢着挪同一个文件
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); int placed = 0; g_img_err[0] = 0; g_img_code[0] = 0; g_img_noshaders[0] = 0; g_img_skipped[0] = 0;
    for (int pass = 0; pass < 2; pass++)   // 先完整包、后更新包（img_place）
        for (int i = 0; i < 2; i++) { char dir[2200]; snprintf(dir, sizeof dir, "%s/%s", proc_docs, s2a_img_import_dirs[i]); img_scan_dir(proc_docs, dir, 0, pass, &placed); }
    pthread_mutex_unlock(&mu); return placed;
}
// 资源还在传（11-02 维护者 测试设备2 暗黑3：推 21.7 GB 中途点开 App，已到的几批被解开、核对时 index.tsv 还没到，报「返回 -8」要玩家删文件夹重拷）。
//   推送契约（tools/push_pack.py）：开推先把机上 manifest.json 置空，各批 tar 推进 .incoming/，index.tsv 与 manifest.json 最后推。所以下面任一条成立 = 这次推送还没完，不能开始解：
//   ① index.tsv / manifest.json 不在或读不出 index_sha256（首次推送清单还没到 / 这次推送开头置空了）；
//   ② 有 tar 没写完（没有归档尾：不足两块、大小不是 512 的倍数、末尾 1024 字节不全零）——在写的包去解会被当截短包，连同后面的包和 .pushed 一起删。
//   原先另有一条「清单的 index_sha256 就是 .applied = 新清单还没到」（11-02 暗黑3，那时推送不置空清单）：置空契约后它与 ① 重复，留着只剩误判——
//   同一份清单重推、推送端用空包盖掉残批时清单没变，永远判「还在传」解不开（iOS 装机剃刀 #24）
//   返回 1 = 还在传，why 写一句原因（日志用）
static int tar_done(const char *p) {
    struct stat st; if (stat(p, &st) || st.st_size < 1024 || st.st_size % 512) return 0;
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint8_t b[1024]; int ok = pread(fd, b, sizeof b, st.st_size - (off_t)sizeof b) == (ssize_t)sizeof b; close(fd);
    for (size_t i = 0; ok && i < sizeof b; i++) ok = !b[i];
    return ok;
}
int s2a_import_transferring(const char *root, char *why, size_t n) {
    char p[2100], sha[200]; struct stat st; why[0] = 0;
    snprintf(p, sizeof p, "%s/index.tsv", root); int idx = !stat(p, &st);
    snprintf(p, sizeof p, "%s/manifest.json", root);
    if (!idx || stat(p, &st)) { snprintf(why, n, "%s还没到", idx ? "manifest.json" : "index.tsv"); return 1; }
    if (manifest_sha(p, sha, sizeof sha)) { snprintf(why, n, "manifest.json 还没写完（读不出 index_sha256）"); return 1; }
    snprintf(p, sizeof p, "%s/.incoming", root); DIR *d = opendir(p); int busy = 0;
    if (d) { struct dirent *e; while (!busy && (e = readdir(d))) { if (!ends_with(e->d_name, ".tar") || !strncmp(e->d_name, "._", 2)) continue;
        char t[2400]; snprintf(t, sizeof t, "%s/%s", p, e->d_name); if (!tar_done(t)) { busy = 1; snprintf(why, n, "%s 还没写完", e->d_name); } } closedir(d); }
    return busy;
}
// 资源包指纹：待导入时看 .incoming 的包、清单，以及推送端开推写的传输清单 .transfer 和它列的文件（同游戏库按 .transfer 判传断了，
//   S2ArmShellHostModel.swift transferStage；原先闸门只看 .incoming，推送直写 game.img 时镜像在长闸门看不见、到点误报停住，iOS 装机剃刀 #32）；
//   指纹在变 = 推送还在往里写。原先没有 / 不完整时还每 3 秒扫整个资源包目录、1 分钟内变过算「正在用访达拷」，随那条路删了（iOS 装机剃刀 #33）
static uint64_t g_fp_n, g_fp_b;
static int fp_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)p; (void)f; if (flag == FTW_F) { g_fp_n++; g_fp_b += (uint64_t)st->st_size; } return 0; }
// .transfer 的 "files": [["相对路径", 字节], …]（tools/push_pack.py json.dumps，非 ASCII 写成 \uXXXX）：逐个把路径解出来交给 fn
static void transfer_files(const char *root, void (*fn)(const char *path)) {
    char tp[2200]; snprintf(tp, sizeof tp, "%s/.transfer", root); FILE *f = fopen(tp, "r"); if (!f) return;
    enum { CAP = 1 << 20 }; char *buf = malloc(CAP); if (!buf) { fclose(f); return; }
    size_t k = fread(buf, 1, CAP - 1, f); fclose(f); buf[k] = 0;
    char *q = strstr(buf, "\"files\""); if (!q) { free(buf); return; }
    while ((q = strstr(q, "[\""))) {
        q += 2; char rel[2048]; size_t o = 0; int ok = 1;
        for (; *q && *q != '"' && ok; q++) {
            unsigned c = (unsigned char)*q;
            if (c == '\\') {
                q++;
                if (*q == 'u') {   // \uXXXX（含代理对）→ UTF-8
                    unsigned cp = (unsigned)strtoul((char[5]){q[1], q[2], q[3], q[4], 0}, NULL, 16); q += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && q[1] == '\\' && q[2] == 'u') { unsigned lo = (unsigned)strtoul((char[5]){q[3], q[4], q[5], q[6], 0}, NULL, 16); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
                    char u[4]; int n = cp < 0x80 ? (u[0] = (char)cp, 1) : cp < 0x800 ? (u[0] = (char)(0xC0 | cp >> 6), u[1] = (char)(0x80 | (cp & 63)), 2)
                        : cp < 0x10000 ? (u[0] = (char)(0xE0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 63)), u[2] = (char)(0x80 | (cp & 63)), 3)
                        : (u[0] = (char)(0xF0 | cp >> 18), u[1] = (char)(0x80 | (cp >> 12 & 63)), u[2] = (char)(0x80 | (cp >> 6 & 63)), u[3] = (char)(0x80 | (cp & 63)), 4);
                    if (o + (size_t)n < sizeof rel) { memcpy(rel + o, u, (size_t)n); o += (size_t)n; } else ok = 0;
                    continue;
                }
                c = (unsigned char)*q;   // \" \\ \/
            }
            if (o + 1 < sizeof rel) rel[o++] = (char)c; else ok = 0;
        }
        rel[o] = 0;
        if (ok && rel[0] && rel[0] != '/' && !strstr(rel, "..")) { char p[4300]; snprintf(p, sizeof p, "%s/%s", root, rel); fn(p); }
    }
    free(buf);
}
static void fp_path(const char *p) { struct stat st; if (!stat(p, &st)) { g_fp_n++; g_fp_b += (uint64_t)st.st_size ^ (uint64_t)st.st_mtime << 20; } }
static uint64_t pack_fp(const char *root) {
    g_fp_n = g_fp_b = 0; char p[2100]; struct stat st;
    const char *top[] = {".incoming", "index.tsv", "manifest.json", ".applied", ".transfer"};
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof p, "%s/%s", root, top[i]); if (i) { if (!stat(p, &st)) fp_one(p, &st, FTW_F, NULL); } else nftw(p, fp_one, 8, FTW_PHYS); }
    transfer_files(root, fp_path);
    return g_fp_n * 0x9E3779B97F4A7C15ULL ^ g_fp_b;
}
// 闸门每隔几秒判一次下一步（iOS 闸门线程按它显示页面）。w 记上次的指纹、它最后变的时刻、是否真变过（首轮不算变）：
//   就绪 → 进游戏；待导入且推送已完 → 导入（推送完没完由契约判，s2a_import_transferring；原先另要指纹稳住 3 秒，白等 3–6 秒，iOS 装机剃刀 #31）；
//   推送没完 → 等（指纹 90 秒没动 → 停住了，请回电脑重装）；没有 / 不完整 → 说明页（原先 1 分钟内变过算「正在用访达拷」再等，随那条路删了）
int s2a_import_decide(const char *docs, struct s2a_gate_watch *w, time_t now, char *why, size_t n) {
    int st = s2a_import_state(docs); why[0] = 0;
    if (st == S2A_IMPORT_READY || st == S2A_IMPORT_PARTIAL) return S2A_GATE_READY;   // 部分可玩：资源可用，玩家确认在 s2a_import_gate 里问
    char root[2048]; s2a_pack_root(root, sizeof root, docs);
    if (st != S2A_IMPORT_PENDING) return st == S2A_IMPORT_BROKEN ? S2A_GATE_BROKEN : S2A_GATE_NONE;
    if (!s2a_import_transferring(root, why, n)) return S2A_GATE_IMPORT;
    uint64_t fp = pack_fp(root);
    if (!w->n++ || fp != w->fp) { w->fp = fp; w->changed = now; }
    return now - w->changed >= S2A_GATE_STALL_S ? S2A_GATE_STALLED : S2A_GATE_WAIT;
}
// 资源包闸门查哪个文档根、查不查（11-07 Duo「游戏没能启动：系统功能没能准备好」定因）：原先只看「S2A_GAME_DATA 没设」才查、且一律查进程级 Documents——
//   多款容器选了游戏后 launchctx.c export_env 总会写 S2A_GAME_DATA → 不查，资源包不在也照起，服务层读 资源包/exefs/main.npdm 抛 DirectoryNotFound、归成 service_init。
//   现在：多款容器 → 查本款文档根（ctx->docs = Documents/games/<游戏号>，资源包在它下面）；单款兼容来源没设 S2A_GAME_DATA → 进程级 Documents（同原先）；
//   其余（单款文件来源 / 开发起局指定了 S2A_GAME_DATA）不查（同原先）。返回 1 = 要查，docs 写进 out；0 = 不查
int s2a_import_docs_pick(const s2a_launch_ctx *c, int game_data_env, const char *proc_docs, char *out, size_t n) {
    if (n) out[0] = 0;
    if (c && c->source == S2A_LCTX_CONTAINER) { snprintf(out, n, "%s", c->docs); return 1; }
    if (game_data_env) return 0;
    snprintf(out, n, "%s", proc_docs ? proc_docs : ""); return 1;
}
// 多款容器的游戏库（app.m s2a_shell_game_install_state / 选游戏前）：本款资源包状态，按同一个判据（s2a_import_state）查 Documents/games/<游戏号>
//   proc_docs = 进程级 Documents（iOS 调用方取 s2a_container_docs），本款文档根 = <proc_docs>/games/<游戏号>（同 launchctx.c s2a_launch_ctx_resolve_game）
int s2a_import_state_game(const char *proc_docs, const char *gid) {
    char docs[1024]; if (!proc_docs || !*proc_docs || !gid || !*gid || strchr(gid, '/') || gid[0] == '.') return S2A_IMPORT_NONE;
    snprintf(docs, sizeof docs, "%s/games/%s", proc_docs, gid);
    return s2a_import_state(docs);
}
// 游戏库快判：解包成功时写的 .applied 等于当前清单 = 这份已核对过每个文件，直接算就绪。
//   库界面在主线程、每次回前台都判，逐文件 stat 一款 10 万文件的游戏要 2.5 秒以上（冷缓存更久，玩家「每次打开都要等」）；
//   完整核对仍在点 Play 后的导入闸门（s2a_import_state）做
// 本体 game.img 在就核长度（D-YGEC 11-17 iOS 0.5.1：本体只剩 5 MB、头里写 3.47 GB，.applied 照样等于清单，快判说就绪，点开服务层报「IMG 没传完」崩）：
//   头里 128 起的「整个文件长」不等于实际长度 = 没传完 / 被截断 → 不算就绪（同 init_fail.c 的判据）。只读 136 字节头；没有 game.img（散文件老包）照旧不管
static int img_whole_or_absent(const char *root) {
    char p[2300]; snprintf(p, sizeof p, "%s/game.img", root);
    int fd = open(p, O_RDONLY); if (fd < 0) return errno == ENOENT;
    uint8_t h[136]; struct stat st = {0}; uint64_t total = 0;
    int ok = pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h, "S2AIMG01", 8) && !fstat(fd, &st) && (memcpy(&total, h + 128, 8), total == (uint64_t)st.st_size);
    close(fd);
    if (!ok) { static int said; if (said++ < 4) LOG("【红】资源包：%s 不完整（实际 %lld 字节，头里写 %llu 字节，或头读不出）：不算就绪，游戏库显示「需要游戏数据」", p, (long long)st.st_size, (unsigned long long)total); }
    return ok;
}
static int applied_ready(const char *root) {   // 资源包目录 root：.applied 等于清单的 index_sha256、程序描述在
    char mf[2300], ap[2300], np[2300], want[200], got[200] = ""; struct stat ns;
    snprintf(mf, sizeof mf, "%s/manifest.json", root); snprintf(ap, sizeof ap, "%s/.applied", root); snprintf(np, sizeof np, "%s/exefs/main.npdm", root);
    FILE *f = fopen(ap, "r");
    if (f) { size_t k = fread(got, 1, sizeof got - 1, f); fclose(f); got[k] = 0; got[strcspn(got, "\r\n")] = 0; }
    // 快判也核程序描述 exefs/main.npdm 在、是普通文件（审计 P2-212：只认 .applied 时删掉它库里仍显示可玩，点开始才报缺）；只 stat 一个文件，不逐文件核
    return got[0] && !manifest_sha(mf, want, sizeof want) && !strcmp(got, want) && !stat(np, &ns) && S_ISREG(ns.st_mode) && ns.st_size > 0 && img_whole_or_absent(root);
}
// 部分可玩（v0.3.1）：解包时只缺非核心文件写的 .partial 第一行 = 当前清单 sha、程序描述在
static int partial_ready(const char *root) {
    char mf[2300], pp[2300], np[2300], want[200], got[2700] = ""; struct stat ns;
    snprintf(mf, sizeof mf, "%s/manifest.json", root); snprintf(pp, sizeof pp, "%s/.partial", root); snprintf(np, sizeof np, "%s/exefs/main.npdm", root);
    FILE *f = fopen(pp, "r"); if (!f) return 0;
    size_t k = fread(got, 1, sizeof got - 1, f); fclose(f); got[k] = 0; got[strcspn(got, "\r\n")] = 0;
    return got[0] && !manifest_sha(mf, want, sizeof want) && !strcmp(got, want) && !stat(np, &ns) && S_ISREG(ns.st_mode) && ns.st_size > 0 && img_whole_or_absent(root);
}
int s2a_import_state_game_quick(const char *proc_docs, const char *gid) {
    if (proc_docs && *proc_docs && gid && *gid && !strchr(gid, '/') && gid[0] != '.') {
        char root[2200], gdocs[1200]; snprintf(gdocs, sizeof gdocs, "%s/games/%s", proc_docs, gid); s2a_pack_root(root, sizeof root, gdocs);
        if (applied_ready(root)) return S2A_IMPORT_READY;
    }
    return s2a_import_state_game(proc_docs, gid);
}
// 游戏库按款的装机阶段（库界面画图块与蒙版用，ios/app.m s2a_shell_game_install_state）。库界面每秒刷新，只做便宜的判断、不逐文件核对：
//   1 就绪——.applied 等于清单、程序描述在（同快判）；
//   2 传完了、只等解开——清单读得出、不在传（11-09 根修 Windows 装完游戏库空：推送开头把清单置空、最后才写，「清单读得出 + 包都写完」= 推送完整）。
//     库在后台跑 s2a_pack_apply 解开（没有待解包时它只核对、写 .applied：只直写了镜像的推送、访达拷进来的也一样），显示「安装中」；核对不过报「安装没完成」；
//   3 传输中——这次推送没完（清单还没写回，或还有包没写完）：有 .transfer 时库按它画进度，没有（老电脑端推的、被删了）时库按资源包里文件的改动判传断了；
//   0 没有（不显示）——只给「资源包」不在或是空的。原先清单读不出 / 在传又没有 .transfer 也给 0，有数据的款从库里静默消失（iOS 装机剃刀 #26 / #45）
int s2a_game_install_state(const char *proc_docs, const char *gid) {
    if (!proc_docs || !*proc_docs || !gid || !*gid || strchr(gid, '/') || gid[0] == '.') return 0;
    char root[2200], mf[2300], want[200], why[256], gdocs[1200];
    snprintf(gdocs, sizeof gdocs, "%s/games/%s", proc_docs, gid); s2a_pack_root(root, sizeof root, gdocs); snprintf(mf, sizeof mf, "%s/manifest.json", root);
    if (applied_ready(root)) return 1;
    if (partial_ready(root)) return 4;   // 部分可玩：库里显示可玩，点开始时闸门问一次（v0.3.1）
    if (manifest_sha(mf, want, sizeof want) || s2a_import_transferring(root, why, sizeof why)) {
        DIR *d = opendir(root); int any = 0;   // 资源包里有没有东西（. 开头的也算：.incoming / .transfer / .applied）
        if (d) { struct dirent *e; while (!any && (e = readdir(d))) any = strcmp(e->d_name, ".") && strcmp(e->d_name, ".."); closedir(d); }
        return any ? 3 : 0;
    }
    return 2;
}
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
// 只有 iOS 起局闸门问「仍要启动」用它（安卓没有这一问，放外面会 -Werror=unused-function）。玩家对这份部分可玩的资源包点过「仍要启动」：.partial_ok = 当时的清单 sha（同一份资源包以后不再问；电脑补推齐了 / 换了包就失效）
static int partial_confirmed(const char *root) {
    char mf[2300], ok[2300], want[200], got[200] = "";
    snprintf(mf, sizeof mf, "%s/manifest.json", root); snprintf(ok, sizeof ok, "%s/.partial_ok", root);
    FILE *f = fopen(ok, "r"); if (!f) return 0;
    size_t k = fread(got, 1, sizeof got - 1, f); fclose(f); got[k] = 0; got[strcspn(got, "\r\n")] = 0;
    return got[0] && !manifest_sha(mf, want, sizeof want) && !strcmp(got, want);
}
#include <dispatch/dispatch.h>
#include <stdatomic.h>
void s2a_ios_show_notice(const char *symbol, const char *title, const char *body, int done, int total, int diag);   // app.m：整屏提示页（notice.m 画）
void s2a_ios_keep_awake(int on);   // app.m：导入期间关自动锁屏（原生层线审：锁屏 = 挂起 = 解包停住）
void s2a_front_run(void (*guest_main)(void)); int s2a_front_is_running(void);
// 提示页文案（11-02 维护者：一句人话说清发生了什么 + 玩家下一步；失败原因、文件名、返回码只进 host.log / unpack.log，经「导出诊断包」带走）。
//   「导入完」请重新打开（原生层线 10-23：不同进程内接着进游戏）。字面在 runtime/include/s2a_ui_text.h（中英两份，S2A_UI 按界面语言取）
static void note_wait(void) { s2a_ios_show_notice("arrow.down.circle.fill", S2A_UI(S2A_UI_IMPORT_WAIT_TITLE), S2A_UI(S2A_UI_IMPORT_WAIT_BODY), 0, 0, 0); }
static void note_stalled(void) { s2a_ios_show_notice("pause.circle.fill", S2A_UI(S2A_UI_IMPORT_STALLED_TITLE), S2A_UI(S2A_UI_IMPORT_STALLED_BODY), 0, 0, 0); }
static void note_done(void) { s2a_ios_show_notice("checkmark.circle.fill", S2A_UI(S2A_UI_IMPORT_DONE_TITLE), S2A_UI(S2A_UI_IMPORT_REOPEN_BODY), 0, 0, 0); }
static void note_entering(void) { s2a_ios_show_notice("checkmark.circle.fill", S2A_UI(S2A_UI_IMPORT_DONE_TITLE), S2A_UI(S2A_UI_IMPORT_ENTERING_BODY), 0, 0, 0); }   // 有人等（预热路径）：导入完当场接着起游戏
static void note_ready_reopen(void) { s2a_ios_show_notice("checkmark.circle.fill", S2A_UI(S2A_UI_IMPORT_READY_TITLE), S2A_UI(S2A_UI_IMPORT_REOPEN_BODY), 0, 0, 0); }
static void note_fail(void) { s2a_ios_show_notice("exclamationmark.triangle.fill", S2A_UI(S2A_UI_IMPORT_FAIL_TITLE), S2A_UI(S2A_UI_IMPORT_FAIL_BODY), 0, 0, 1); }
static const char *help_title(int st) { return st == S2A_IMPORT_BROKEN || st == S2A_IMPORT_PARTIAL ? S2A_UI(S2A_SELFCHECK_PKG_TITLE) : S2A_UI(S2A_UI_PACK_MISSING_TITLE); }   // 说明页标题
static char g_imp_docs[1024], g_imp_name[256];
// 缺资源包 / 资源包不全的说明页（app.m s2a_ios_show_pack_help：示意图 + 三步 + 「我已放好，重新检查」）。10-26 维护者：「壳上做好说明别闪退，
//   打开发现缺资源包就告诉用户把启动器导出的资源包导到手机上指定位置」
void s2a_ios_show_pack_help(const char *title, const char *detail, const char *app, void (*recheck)(void));
void s2a_ios_hide_pack_help(void); void s2a_ios_hide_notice(void);
// 闸门线程（11-02 根修「资源没传完就打开 App」）：每 3 秒按 s2a_import_decide 判一次——还在传就说「还没传完、传完自动继续」，传完自己开始导入；
//   不再一看到待导入的包就解（推送中途解 = 清单没到报「返回 -8」/ 在写的包被当截短包删掉）。说明页的「重新检查」= 立刻再判一次
static dispatch_semaphore_t g_poll_sem, g_ready_sem; static _Atomic int g_gate_waiting;
static void gate_recheck(void) { dispatch_semaphore_signal(g_poll_sem); }
static void imp_progress(int done, int total) { s2a_ios_show_notice("shippingbox.fill", S2A_UI(S2A_UI_IMPORT_PROGRESS_TITLE), S2A_UI(S2A_UI_IMPORT_PROGRESS_BODY), done, total, 0); }
static void gate_show(int act, int again) {
    if (act == S2A_GATE_WAIT) { note_wait(); return; }
    if (act == S2A_GATE_STALLED) { note_stalled(); return; }
    int st = act == S2A_GATE_BROKEN ? S2A_IMPORT_BROKEN : S2A_IMPORT_NONE; char d[1024]; s2a_pack_help_detail(st, again, d, sizeof d);
    s2a_ios_show_pack_help(help_title(st), d, g_imp_name, gate_recheck);
}
static void *gate_thread(void *a) { (void)a;
    static const char *const act_name[] = {"就绪", "导入", "等包稳住", "资源还在传", "资源传输停住了", "没有游戏资源", "资源包不完整"};
    struct s2a_gate_watch w = {0}; int last = -1, poked = 0;
    for (;;) {
        char why[600]; int act = s2a_import_decide(g_imp_docs, &w, time(NULL), why, sizeof why);
        if (act == S2A_GATE_READY) {
            // 预热路径：宿主线程在 s2a_import_gate 里等 → 收起页面、接着起游戏；老路径（闸门在主线程、UIKit 接管后不返回）没人等 → 请玩家重新打开。
            //   有没有人等在起线程前就定了（s2a_import_gate），不再在这里等 2 秒看宿主线程到没到（iOS 装机剃刀 #34：时间补丁，没赶上就误走「请重新打开」）
            s2a_ios_keep_awake(0); s2a_ios_hide_pack_help();
            if (atomic_load(&g_gate_waiting)) { s2a_ios_hide_notice(); LOG("资源导入：就绪，收起页面、接着起游戏"); dispatch_semaphore_signal(g_ready_sem); }
            else { LOG("资源导入：就绪，提示玩家重新打开"); note_ready_reopen(); }
            return NULL;
        }
        if (act == S2A_GATE_IMPORT) {
            LOG("资源导入：推送已完、包已稳住，后台解开");
            s2a_ios_keep_awake(1); imp_progress(0, 1); s2a_pack_apply_progress = imp_progress; int rc = s2a_pack_apply(g_imp_docs); s2a_pack_apply_progress = NULL;
            // 导入完回去再判一次：就绪 → 宿主线程在等（预热路径）就当场接着起游戏，没人等（老路径）才请玩家重新打开（11-02 用户体验线：首启导入完还要划掉重开是硬伤；
            //   10-23 定「重新打开」时闸门还在主线程、UIKit 接管后不返回，接不下去；10-26 预热路径能接了，这一支没跟上）。解包是流式的、文件都关了，不留占用
            //   页面也分两路：有人等就说「正在进入游戏」，「划掉重开」只留给没人等的老路径（11-02 用户体验线：成功路径先闪一句叫玩家划掉，紧接着游戏自己进去了）
            if (!rc) { LOG("资源导入：完成，再判一次就绪（有人等就接着起游戏）"); if (atomic_load(&g_gate_waiting)) note_entering(); else note_done(); w = (struct s2a_gate_watch){0}; last = -1; continue; }
            // 解到一半又来了新推送（执行器重推 / 又拷进一份）：回去等它传完再试，不报失败
            char root[2048], w2[600]; s2a_pack_root(root, sizeof root, g_imp_docs);
            if (s2a_import_state(g_imp_docs) == S2A_IMPORT_PENDING && s2a_import_transferring(root, w2, sizeof w2)) {
                LOG("资源导入：这次没解成（%s），但又有资源在传（%s）：等传完自动重试", s2a_pack_apply_failure(), w2); w = (struct s2a_gate_watch){0}; last = -1; continue; }
            s2a_ios_keep_awake(0); note_fail(); LOG("【红】资源导入：失败 %s", s2a_pack_apply_failure());
            return NULL;   // 不退出进程：UIKit 留着显示结果（玩家看到 _exit 会以为闪退）
        }
        if (act != last || poked) {
            if (act != last) LOG("资源导入：%s%s%s", act_name[act], why[0] ? "：" : "", why);
            gate_show(act, poked && act == last); s2a_ios_keep_awake(act == S2A_GATE_WAIT); last = act;
        }
        poked = !dispatch_semaphore_wait(g_poll_sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    }
}
// host_main.c 自检之后、占地址窗口之前调（只在没指定 S2A_GAME_DATA 时，即玩家正常打开）：就绪直接返回 0；否则起闸门线程显示页面、等它判就绪。
//   预热路径（UIKit 已在主线程，本函数在宿主线程）里 s2a_front_run 立即返回：等到就绪就返回 1 接着起游戏（当场进）；老路径里 s2a_front_run 不返回。
//   返回 1 = 本局资源是闸门当场解开的：起局前读过的随包补丁 / 能力清单读的是解开前的资源包，调用方要重读（审计 P1-106）
static int imp_docs(char *out, size_t n) {   // 本局资源包闸门的文档根；0 = 本局不查（见 s2a_import_docs_pick）
    char proc[1024]; s2a_container_docs(proc, sizeof proc);
    return s2a_import_docs_pick(s2a_launch_ctx_get(), getenv("S2A_GAME_DATA") != NULL, proc, out, n);
}
int s2a_import_gate_applies(void) { char d[1024]; return imp_docs(d, sizeof d); }
// 资源包只缺非核心文件（v0.3.1 缺件照玩）：问玩家「仍要启动」。同一份资源包点过一次就记下（.partial_ok），不再问。
//   本线程等玩家点（s2a_ios_ask_continue 只在宿主工作线程上能等）；返回 1 = 放行，0 = 问不了（主线程上，调用方照旧走闸门）
int s2a_ios_ask_continue(const char *symbol, const char *title, const char *body, const char *go);   // ios/app.m
static int partial_ask(void) {
    char root[2048], mf[2300], sha[200]; s2a_pack_root(root, sizeof root, g_imp_docs);
    if (partial_confirmed(root)) { LOG("资源导入：缺 %d 个非必需文件，这份资源包玩家已确认过，照常起局", g_pack_bad); return 1; }
    if (!s2a_ios_ask_continue("exclamationmark.triangle.fill", S2A_UI(S2A_UI_PACK_PARTIAL_TITLE), S2A_UI(S2A_UI_PACK_PARTIAL_ASK), S2A_UI(S2A_UI_PACK_PARTIAL_GO))) return 0;
    snprintf(mf, sizeof mf, "%s/manifest.json", root);
    if (!pack_in_bundle(root, "记「仍要启动」") && !manifest_sha(mf, sha, sizeof sha) && write_atomic(root, ".partial_ok", sha)) LOG("【黄】资源导入：记不下「已确认」（%s），下次还会再问", strerror(errno));
    LOG("资源导入：缺 %d 个非必需文件（如 %s），玩家点了「仍要启动」，照常起局；真读到缺的文件时游戏 Break → 说明页", g_pack_bad, g_pack_first);
    return 1;
}
int s2a_import_gate(void) {
    if (!imp_docs(g_imp_docs, sizeof g_imp_docs)) return 0;
    s2a_pack_migrate_one(g_imp_docs);   // 直接起这款（没经过游戏库）也先把旧位置的资源包挪过去（v0.3.0）
    CFStringRef dn = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("CFBundleDisplayName"));
    if (!dn || !CFStringGetCString(dn, g_imp_name, sizeof g_imp_name, kCFStringEncodingUTF8)) strlcpy(g_imp_name, "S2Arm", sizeof g_imp_name);
    int st = s2a_import_state(g_imp_docs);
    if (st == S2A_IMPORT_READY) return 0;
    if (st == S2A_IMPORT_PARTIAL && s2a_front_is_running() && partial_ask()) return 0;   // 只缺非核心文件：问一次「仍要启动」，点了照常起局
    LOG("资源导入：资源没就绪（状态 %d），起闸门线程每 3 秒判一次（UIKit 先起来，免得被启动看门狗杀）", st);
    g_poll_sem = dispatch_semaphore_create(0); g_ready_sem = dispatch_semaphore_create(0);
    // 有没有人等：界面已在跑（预热路径）→ 下面 s2a_front_run 立即返回、本线程等闸门；否则 s2a_front_run 进 UIApplicationMain 不返回，没人等
    atomic_store(&g_gate_waiting, s2a_front_is_running());
    pthread_t th; if (pthread_create(&th, NULL, gate_thread, NULL)) die("资源导入线程起不来");
    s2a_front_run(NULL);
    dispatch_semaphore_wait(g_ready_sem, DISPATCH_TIME_FOREVER); atomic_store(&g_gate_waiting, 0);
    if (s2a_import_state(g_imp_docs) == S2A_IMPORT_PARTIAL) partial_ask();   // 当场解开后发现只缺非核心文件：同样先问
    return 1;
}
// main() 起 UIKit 前问：资源包没就绪（要显示说明页 / 导入进度）就走延后启动，闸门在宿主线程上等「重新检查」
int s2a_import_needs_ui(void) {
    char docs[1024]; if (!imp_docs(docs, sizeof docs)) return 0;
    s2a_pack_migrate_one(docs);   // 起 UIKit 前判就绪：先迁（同 s2a_import_gate），免得旧位置有资源被判成「没有」
    return s2a_import_state(docs) != S2A_IMPORT_READY;
}
// 游戏中途因缺数据 Break（lifecycle.c 收局前问）：资源包这时核不过 = 是缺资源，不退出，弹同一页；游戏线程已坏、接不上，重新检查就绪后请玩家重新打开。
//   返回 1 = 已接管（调用方不退出，本线程停在这里）；资源包是好的返回 0（照常退出，是别的原因）
static void break_recheck(void) {
    int st = s2a_import_state(g_imp_docs);
    if (st == S2A_IMPORT_READY) { s2a_ios_hide_pack_help(); note_ready_reopen(); return; }
    char d[1024]; s2a_pack_help_detail(st, 1, d, sizeof d); s2a_ios_show_pack_help(help_title(st), d, g_imp_name, break_recheck);
}
int s2a_pack_help_on_break(void) {
    if (!imp_docs(g_imp_docs, sizeof g_imp_docs)) s2a_container_docs(g_imp_docs, sizeof g_imp_docs);   // 不查闸门的局（开发指定资源包）照旧核进程级 Documents
    if (!g_imp_name[0]) { CFStringRef dn = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("CFBundleDisplayName"));
        if (!dn || !CFStringGetCString(dn, g_imp_name, sizeof g_imp_name, kCFStringEncodingUTF8)) strlcpy(g_imp_name, "S2Arm", sizeof g_imp_name); }
    int st = s2a_import_state(g_imp_docs); if (st == S2A_IMPORT_READY) return 0;
    LOG("【红】游戏 Break 时资源包%s：不退出，显示说明页", st == S2A_IMPORT_BROKEN ? "不完整" : "不在");
    char d[1024]; s2a_pack_help_detail(st, 0, d, sizeof d); s2a_ios_show_pack_help(help_title(st), d, g_imp_name, break_recheck);
    return 1;
}
#endif
