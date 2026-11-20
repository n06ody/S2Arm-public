// 自检提示单测（10-23 E01 iPad mini 6：权限签了却提示漏签）：权限 XML 解析 + 按事实给提示，期望句子手写
#include "../src/native.h"
#include <stdio.h>
#include <string.h>
int s2a_ent_true(const char *xml, size_t n, const char *key);
void s2a_selfcheck_explain(char *out, size_t cap, int win_ok, int mem_ok, int ent, uint64_t avail_mb, uint64_t phys_mb, uint64_t va_top, uint64_t win_mb, uint64_t need_mb, int as_type);
int s2a_selfcheck_mem_blocks(int mem_ok, int ent);
void s2a_selfcheck_screen(int win_ok, int mem_ok, int ent, uint64_t need_mb, int as_type, const char **title, const char **body);
static int bad;
static void eqi(int got, int want, const char *what) { if (got != want) { printf("✗ %s：%d，应 %d\n", what, got, want); bad++; } else printf("✓ %s\n", what); }
static void eqs(const char *got, const char *want, const char *what) { if (strcmp(got, want)) { printf("✗ %s：\n  实：%s\n  应：%s\n", what, got, want); bad++; } else printf("✓ %s\n", what); }
int main(void) {
    const char *K = "com.apple.developer.kernel.increased-memory-limit";
    const char *x1 = "<plist><dict><key>get-task-allow</key><true/><key>com.apple.developer.kernel.increased-memory-limit</key>\n\t<true/></dict></plist>";
    const char *x2 = "<dict><key>com.apple.developer.kernel.increased-memory-limit</key><false/></dict>";
    const char *x3 = "<dict><key>com.apple.developer.kernel.increased-memory-limit-x</key><true/></dict>";
    const char *x4 = "<dict><key>get-task-allow</key><true/></dict>";
    eqi(s2a_ent_true(x1, strlen(x1), K), 1, "权限为 true（中间有换行和制表符）");
    eqi(s2a_ent_true(x2, strlen(x2), K), 0, "权限为 false");
    eqi(s2a_ent_true(x3, strlen(x3), K), 0, "只有前缀相同的别的 key");
    eqi(s2a_ent_true(x4, strlen(x4), K), 0, "没有这个 key");
    eqi(s2a_ent_true(x1, 60, K), 0, "截断的 XML 不越界、不误判");
    char o[640];
    s2a_selfcheck_explain(o, sizeof o, 1, 1, 1, 6144, 12288, 0x8000000000ull, 65536, 32768, -1);
    eqs(o, "自检通过", "都够");
    s2a_selfcheck_explain(o, sizeof o, 0, 0, 0, 3376, 12288, 0x7180000000ull, 0, 32768, -1);
    eqs(o, "安装时缺少『提高内存上限』权限，请用 AltStore / Xcode 个人团队重装或用 GetMoreRam 补开", "真没签：叫重签");
    s2a_selfcheck_explain(o, sizeof o, 0, 0, 1, 2865, 3747, 0x7180000000ull, 0, 32768, -1);
    eqs(o, "已签『提高内存上限』权限，本款需要 32 GB 地址空间，本机最多 0 GB（系统给本进程的地址上限 0x7180000000）；系统给本进程的内存上限 2865 MB，起局需要 ≥ 4500 MB。这是本机型（物理内存 3747 MB）的系统上限，重装或补权限不能解决，本机型目前不支持", "已签、地址与内存都不够（E01 情形）");
    // 10-24 #61 iPadOS 26.6.1：最大一块 51 GB（52224 MB），系统资源非 0 的款要 64768 MB（63.25 GB，向上取整 64 GB）→ 写清要多少、有多少；内存够 → 不说机型不支持
    s2a_selfcheck_explain(o, sizeof o, 0, 1, 1, 6144, 8192, 0xfc0000000ull, 52224, 64768, -1);
    eqs(o, "已签『提高内存上限』权限，本款需要 64 GB 地址空间，本机最多 51 GB（系统给本进程的地址上限 0xfc0000000）。重装或补权限不能解决", "iPad 26：大跨度款放不下");
    s2a_selfcheck_explain(o, sizeof o, 1, 0, 1, 2865, 3747, 0x8000000000ull, 65536, 32768, -1);
    eqs(o, "已签『提高内存上限』权限，系统给本进程的内存上限 2865 MB，起局需要 ≥ 4500 MB。这是本机型（物理内存 3747 MB）的系统上限，重装或补权限不能解决，本机型目前不支持", "已签、只有内存不够");
    s2a_selfcheck_explain(o, sizeof o, 0, 1, -1, 6144, 8192, 0, 0, 32768, -1);
    eqs(o, "读不到本进程的权限（按已签处理），本款需要 32 GB 地址空间，本机最多 0 GB（系统给本进程的地址上限 0）。重装或补权限不能解决", "权限读不到、地址上限读不到");
    s2a_selfcheck_explain(o, sizeof o, 0, 1, 1, 6144, 8192, 0x8000000000ull, 65536, 0, -1);
    eqs(o, "已签『提高内存上限』权限，读不到本款的 main.npdm，不知道要多大的地址空间（包不完整）。重装或补权限不能解决", "包里没有 main.npdm");
    // 屏上那句（11-02 维护者：人话 + 下一步，不出现地址上限 / main.npdm / MB）：期望按三种事实手写
    const char *t, *b;
    s2a_selfcheck_screen(0, 0, 0, 32768, -1, &t, &b); eqs(t, "安装少了一项必要权限", "屏上：真没签 → 标题"); eqi(!!strstr(b, "续签 S2Arm"), 1, "屏上：真没签 → 叫重装");
    s2a_selfcheck_screen(0, 1, 1, 0, -1, &t, &b); eqs(t, "游戏资源不完整", "屏上：读不到 main.npdm → 资源不完整");
    // v0.3.1：窗口占到、只差内存上限（没签权限）→ 同一标题，正文换成「可以先仍要启动、风险是什么、怎么彻底解决」
    s2a_selfcheck_screen(1, 0, 0, 32768, -1, &t, &b); eqs(t, "安装少了一项必要权限", "屏上：只差内存上限 → 标题照旧");
    eqi(!!strstr(b, "仍要启动") && !!strstr(b, "被系统结束") && !!strstr(b, "续签 S2Arm"), 1, "屏上：只差内存上限 → 正文给出可选的两条路和风险");
    s2a_selfcheck_screen(0, 0, 1, 32768, -1, &t, &b); eqs(t, "这台手机暂时玩不了这款游戏", "屏上：签了仍不够 → 本机型不支持");
    eqi(!strstr(b, "MB") && !strstr(b, "0x") && !strstr(b, "npdm"), 1, "屏上：不出现 MB / 地址 / npdm");
    // 11-06 起类型 1（36 位）布局支持：跨度为 0 就是读不到模块段，同类型 3 照资源不完整说；类型 0 / 2（32 位）照实说暂不支持。期望句子手写，不取宏
    s2a_selfcheck_screen(0, 1, 1, 0, 1, &t, &b); eqs(t, "游戏资源不完整", "屏上：类型 1 跨度 0 → 资源不完整（36 位已支持，不再说暂不支持）");
    s2a_selfcheck_screen(0, 1, 1, 0, 2, &t, &b);
    eqs(t, "暂不支持这款游戏", "屏上：类型 2 → 标题「暂不支持这款游戏」");
    eqs(b, "这款游戏使用 32 位地址空间，S2Arm 暂不支持，重新安装或换手机都解决不了。", "屏上：类型 2 → 正文说 32 位地址空间暂不支持");
    eqi(!strstr(b, "NPDM") && !strstr(b, "npdm") && !strstr(b, "类型"), 1, "屏上：类型 2 → 不出现 NPDM / 类型 这类内部词");
    s2a_selfcheck_screen(0, 0, 0, 0, 2, &t, &b); eqs(t, "暂不支持这款游戏", "屏上：类型 2 且没签权限 → 仍说暂不支持（补权限也解决不了）");
    s2a_selfcheck_screen(0, 1, 1, 0, 0, &t, &b); eqi(!!strstr(b, "32 位地址空间"), 1, "屏上：类型 0 → 正文说 32 位地址空间");
    s2a_selfcheck_screen(0, 1, 1, 0, 3, &t, &b); eqs(t, "游戏资源不完整", "屏上：类型 3 跨度 0 → 仍是资源不完整（原措辞）");
    s2a_selfcheck_screen(0, 1, 1, 0, 5, &t, &b); eqs(t, "游戏资源不完整", "屏上：类型 5（非法值）→ 资源不完整");
    s2a_selfcheck_explain(o, sizeof o, 0, 1, 1, 6144, 8192, 0x8000000000ull, 65536, 0, 0);
    eqs(o, "本款 NPDM 地址空间类型 0（32 位）暂不支持（布局只支持类型 1 = 36 位、3 = 39 位），重装、补权限或换机型都不能解决", "日志：类型 0 → 照实说 32 位暂不支持");
    s2a_selfcheck_explain(o, sizeof o, 0, 1, 1, 6144, 8192, 0x8000000000ull, 65536, 0, 1);
    eqs(o, "已签『提高内存上限』权限，读不到本款的 main.npdm，不知道要多大的地址空间（包不完整）。重装或补权限不能解决", "日志：类型 1 跨度 0 → 同类型 3 的原措辞");
    s2a_selfcheck_explain(o, sizeof o, 0, 1, 1, 6144, 8192, 0x8000000000ull, 65536, 0, 3);
    eqs(o, "已签『提高内存上限』权限，读不到本款的 main.npdm，不知道要多大的地址空间（包不完整）。重装或补权限不能解决", "日志：类型 3 跨度 0 → 原措辞");
    // #29 内存门槛只拦没签权限的（手算：签了 / 读不到都放行；够的不拦）
    eqi(s2a_selfcheck_mem_blocks(0, 0), 1, "内存不够 + 没签权限 → 拦");
    eqi(s2a_selfcheck_mem_blocks(0, 1), 0, "内存不够 + 已签 → 放行");
    eqi(s2a_selfcheck_mem_blocks(0, -1), 0, "内存不够 + 读不到权限（按已签）→ 放行");
    eqi(s2a_selfcheck_mem_blocks(1, 0), 0, "内存够 + 没签 → 不因内存拦");
    printf(bad ? "✗ t_selfcheck：%d 处不符\n" : "✓ t_selfcheck：0 处不符\n", bad);
    return bad != 0;
}
