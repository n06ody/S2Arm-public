// 一次性诊断（不进 PR）：android/native/gfx_plat/spv_samp_use.h 的取样用法分类单测。
// 用法：spv_samp_use_test <cases.tsv> <SPIR-V 目录>；cases.tsv 每行「文件 set binding 期望分类 期望原因」，
//   文件是 glslangValidator / spirv-as 产出的 .spv（runner 先生成），@header / @truncated 是本程序合成的坏模块
#include "../native/gfx_plat/spv_samp_use.h"
#include <stdio.h>

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(l > 0 ? (size_t)l : 1); *n = l > 0 && fread(b, 1, (size_t)l, f) == (size_t)l ? (size_t)l : 0; fclose(f); return b;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *c = fopen(argv[1], "r"); if (!c) { printf("✗ 读不到 %s\n", argv[1]); return 2; }
    char line[512]; int bad = 0, n = 0;
    while (fgets(line, sizeof line, c)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char file[128], want[32], extra[64]; unsigned set, binding;
        if (sscanf(line, "%127s %u %u %31s %63s", file, &set, &binding, want, extra) != 5) { printf("✗ 坏行：%s", line); bad++; continue; }
        unsigned char *b = NULL; size_t len = 0; uint32_t hdr[6] = {0x07230203u, 0x00010000u, 0, 100, 0, 0};
        if (!strcmp(file, "@header")) { b = malloc(24); memcpy(b, hdr, 24); ((uint32_t *)b)[0] = 0x12345678u; len = 24; }
        else if (!strcmp(file, "@truncated")) { hdr[5] = (9u << 16) | 59u; b = malloc(24); memcpy(b, hdr, 24); len = 24; }   // 指令说 9 个字、只剩 1 个
        else { char p[1024]; snprintf(p, sizeof p, "%s/%s.spv", argv[2], file); b = slurp(p, &len); if (!b || !len) { printf("✗ 读不到 %s\n", p); bad++; free(b); continue; } }
        struct spv_su r; spv_su_classify(b, len, set, binding, &r); free(b); n++;
        char got[64]; snprintf(got, sizeof got, "why=%s", spv_su_why_name(r.why)); char gop[32]; snprintf(gop, sizeof gop, "op=%u", (unsigned)r.op);
        int ok = !strcmp(spv_su_cls_name(r.cls), want) && (!strcmp(extra, "-") || !strcmp(extra, got) || !strcmp(extra, gop));
        printf("%s %s set=%u binding=%u：期望 %s %s → %s %s %s fetch=%u query=%u sample=%u\n", ok ? "✓" : "✗", file, set, binding, want, extra,
               spv_su_cls_name(r.cls), got, gop, (unsigned)r.fetch, (unsigned)r.query, (unsigned)r.sample);
        if (!ok) bad++;
    }
    fclose(c);
    if (bad || !n) { printf("✗ spv_samp_use：%d 例里 %d 例不符\n", n, bad); return 1; }
    printf("✓ spv_samp_use：%d 例全符\n", n); return 0;
}
