// 多模块装载（扮演 Horizon 的 rtld）：映射、门登记、跨模块重定位、段权限。旧库 loader.c 改后搬：
// 删掉按名接管（强制接管表 / nn::fs 入口改道）、宽屏与代码字补丁（按游戏写死的点）、游戏身份门；装载不改任何游戏代码。
// 代码区一拷进来就设成只读+执行，此后装载器只写 data：iOS 上代码区是签进 App 的节，原样 remap 过来。
// 随包表：gpu_drivers.tsv（出包组装 pack/ship_ext.py 从产物根拷进 App，runtime/build.mk 按这行自动收清单）
// 随包表：nro_modules.tsv（出包组装 pack/ship_ext.py 从产物根拷进 App，runtime/build.mk 按这行自动收清单；ro LoadNro 按 NRO 哈希查改写映像，11-02 前 iOS 漏拷：马力欧派对第一次装 NRO 即中止）
#include "../include/s2a_docs.h"
#include <stdatomic.h>
#include "native.h"
#include "loader.h"
#include "s2a_gfx_mode.h"
#include "ro_find.h"
#include "../../platform/plat.h"
#if !defined(__ANDROID__)
#include <TargetConditionals.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include "../../android/native/module.h"
#else
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dirent.h>
#endif

module_t *g_mod;
int g_nmod, g_mod_cap;
// Only the outer table readers and raw image copy span calls that may die.
// Their inputs belong to this boot thread until close/copy completes.
static _Thread_local FILE *g_boot_file;
static _Thread_local int g_boot_fd = -1;
static _Thread_local uint8_t *g_boot_raw;
#if defined(__APPLE__)
// try_boot owns an empty loader until the complete native boot returns. No
// guest, managed session or graphics consumer has been started in this scope.
static _Thread_local int g_boot_owned;
static _Thread_local struct boot_map { uint64_t at, length; } *g_boot_maps;   // g_mod_cap 项，mod_table_alloc 在持有者线程上分配
int s2a_loader_begin_boot(void) {
    if (g_boot_owned || g_nmod || g_mod || g_boot_maps) return -1;   // 上一局的模块表没释放 = 不是空装载器
    g_boot_owned = 1;
    return 0;
}
void s2a_loader_commit_boot(void) { free(g_boot_maps); g_boot_maps = NULL; g_boot_owned = 0; }
// Each boot slot gets one allocation. Check before allocating so a nested
// NRO reload cannot overwrite ownership of an earlier successful mapping.
static int boot_map_slot(module_t *m) {
    if (!g_boot_owned) return -1;
    int slot = (int)(m - g_mod);
    if (!g_boot_maps || slot < 0 || slot >= g_mod_cap || g_boot_maps[slot].length) die("失败装载所有权槽重复或越界");
    return slot;
}
static void boot_map_record(int slot, uint64_t at, uint64_t length) {
    if (slot >= 0) { g_boot_maps[slot].at = at; g_boot_maps[slot].length = length; }
}
#endif
void s2a_loader_abort_boot_inputs(void) {
    FILE *file = g_boot_file; g_boot_file = NULL;
    int fd = g_boot_fd; g_boot_fd = -1;
    uint8_t *raw = g_boot_raw; g_boot_raw = NULL;
    if (file && fclose(file)) LOG("【红】失败装载的临时表关闭失败 errno=%d", errno);
    if (fd >= 0 && close(fd)) LOG("【红】失败装载的临时映像 fd 关闭失败 errno=%d", errno);
    free(raw);
}
// 模块表按本局要的个数一次分配（native.h 模块表注释）：之后只按下标填、不搬家
static void mod_table_alloc(int cap) {
    if (g_mod || g_nmod) die("模块表已分配（上一局没有复位）");
    if (cap <= 0) die("模块表容量 %d 无效", cap);
    if (!(g_mod = calloc((size_t)cap, sizeof *g_mod))) die("模块表 %d 项分配失败", cap);
#if defined(__APPLE__)
    if (g_boot_owned && !(g_boot_maps = calloc((size_t)cap, sizeof *g_boot_maps))) die("失败装载所有权表 %d 项分配失败", cap);
#endif
    g_mod_cap = cap;
}
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };
static const char *const k_keyname[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
static pthread_key_t key_value(unsigned i) { return i == KEY_K18 ? g_key18 : i == KEY_KTLS ? g_keytls : i == KEY_KTLR ? g_keytlr : g_keygate; }

enum { DT_INIT = 12, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5, DT_RELRSZ = 35, DT_RELR = 36 };
#include "reloc_class.h"   // 重定位分类（含 TLS 留给 nnmusl）与类型号
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)

static void create_keys(void) {
    static int ready; if (ready) return;
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("创建线程槽键失败");
    LOG("键号 K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", (unsigned long)g_key18, (unsigned long)g_keytls, (unsigned long)g_keytlr, (unsigned long)g_keygate);
    ready = 1;
}
// 槽 = TSD 基址 + 平台偏移（Apple key×8；Android Bionic key 表偏移），起局核一次
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base = plat_tsd_base(), off;
    if (!base || plat_tsd_offset((uint32_t)g_key18, &off)) die("TSD 布局：取基址 / 偏移失败");
    if (*(uint64_t *)(base + off) != 0x1234) die("TSD 布局不符");
    pthread_setspecific(g_key18, 0);
}

// ---------- 符号 ----------
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
// 名字哈希表：按下标升序插入、同名已在则不覆盖 ⇒ 查询结果 = 线性扫描的首个已定义同名符号
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;
        }
    }
}
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: 没有 MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}
static const Elf64_Sym *module_sym_ent(const module_t *m, const char *name) {   // 符号表条目（要 st_size 的地方用）；找不到回 NULL
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) return s;
    }
    return NULL;
}
void *module_sym(module_t *m, const char *name) { const Elf64_Sym *s = module_sym_ent(m, name); return s ? m->base + s->st_value : NULL; }
void *guest_sym(const char *name) { for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; } return NULL; }
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if ((g_mod[i].patch_off < 0 || g_mod[i].patch2_len) && s2a_in_patch(&g_mod[i], (uint64_t)p)) return &g_mod[i];   // 基址前 / 键页后的补丁岛
    }
    return NULL;
}

// ---------- 改写器随行表 ----------
static char g_main_build_id[40];
const char *s2a_main_build_id(void) { return g_main_build_id; }
// 金手指目录给 C#（services Cheats.cs）：有界面入口的宿主（ios/vpad_cheats.m 装载时）先登记，才回 <文档根>/cheats（s2a_docs.h 唯一取法）；
//   没登记（Mac / 安卓，没有金手指界面）回空串，C# 就不起线程、不碰用户文稿夹。平台差异落在平台层登记这一步，共享代码不判平台
static _Atomic int g_cheats_host;
void s2a_cheats_host_enable(void) { atomic_store(&g_cheats_host, 1); }
const char *s2a_cheats_dir(void) { static char d[1024]; if (!atomic_load(&g_cheats_host)) return ""; s2a_docs_path(d, sizeof d, "cheats"); return d; }
uint64_t s2a_main_base(void) { return g_nmod ? (uint64_t)(uintptr_t)g_mod[0].base : 0; }   // 主程序（Horizon 序第一个模块）装载基址：金手指 Atmosphere「main」区（services Cheats.cs）
// 同一表解析器读取目录流或已验证 APK 的借用字节；后者不落地、也不二次打开。
struct loader_text {
    FILE *file;
    const unsigned char *bytes;
    size_t length, cursor;
    const char *what;
};
static int text_line(struct loader_text *text, char *line, size_t capacity) {
    if (text->file) {
        if (!fgets(line, (int)capacity, text->file)) {
            if (ferror(text->file)) die("读 %s 失败", text->what);
            return 0;
        }
#if defined(__ANDROID__)
        if (!strchr(line, '\n') && !feof(text->file)) die("%s: 表行过长", text->what);
#endif
        return 1;
    }
    if (text->cursor == text->length) return 0;
    size_t start = text->cursor, end = start;
    while (end < text->length && text->bytes[end] != '\n') {
        if (!text->bytes[end]) die("%s: 表中包含 NUL", text->what);
        end++;
    }
    size_t length = end - start;
    if (length >= capacity) die("%s: 表行过长", text->what);
    memcpy(line, text->bytes + start, length); line[length] = 0;
    text->cursor = end < text->length ? end + 1 : end;
    return 1;
}
static int far_cmp(const void *a, const void *b) { int64_t x = ((const struct s2a_far *)a)->from, y = ((const struct s2a_far *)b)->from; return x < y ? -1 : x > y; }
static void read_layout_text(module_t *m, struct loader_text *text) {
    const char *path = text->what;
    char line[256], key[32]; unsigned long v; unsigned seen = 0, trap_cap = 0, far_cap = 0; int have_traps = 0, have_fars = 0;
#if defined(__ANDROID__)
    int android_target = 0, android_abi = 0;
#endif
    while (text_line(text, line, sizeof line)) {
        if (!strncmp(line, "# BUILD_ID\t", 11)) { if (!strcmp(m->name, "main")) { strlcpy(g_main_build_id, line + 11, sizeof g_main_build_id); g_main_build_id[strcspn(g_main_build_id, "\r\n")] = 0; } continue; }
        // 非苹果目标（S2A_TARGET=android 等）的产物线程槽取法不同：补丁代码读 tpidr_el0，在这里跑会读错槽。苹果产物没有这一行
        if (!strncmp(line, "# TARGET\t", 9)) {
#if defined(__ANDROID__)
            size_t len = strcspn(line + 9, "\r\n");
            if (android_target || len != 7 || memcmp(line + 9, "android", 7))
                die("%s: 必须是唯一 TARGET android 产物", path);
            android_target = 1; continue;
#else
            die("%s: 改写目标是 %.*s，不是苹果，用缺省 S2A_TARGET 重出", path, (int)strcspn(line + 9, "\r\n"), line + 9);
#endif
        }
#if defined(__ANDROID__)
        if (!strncmp(line, "# REWRITE_ABI\t", 14)) {
            static const char expected[] = "android-sp-safe-v1";
            size_t len = strcspn(line + 14, "\r\n");
            if (android_abi || len != sizeof expected - 1 || memcmp(line + 14, expected, sizeof expected - 1))
                die("%s: 必须是唯一 REWRITE_ABI android-sp-safe-v1 产物", path);
            android_abi = 1; continue;
        }
#endif
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: 表头以外有内容（旧改写器产物），用 runtime/rewriter 重出", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);
        else if (!strcmp(key, "PATCH2_OFF")) m->patch2_off = strtoll(line + 2 + strlen(key), NULL, 16);
        else if (!strcmp(key, "PATCH2_LEN")) m->patch2_len = v;
        else if (!strcmp(key, "TRAP_SITES")) {   // 随后 n 行 # TRAP（序号 = 出现顺序）
            unsigned long n = strtoul(line + 2 + strlen(key), NULL, 10);
            if (m->traps || n > S2A_TRAP_MAX) die("%s: TRAP_SITES 重复或超过 %#x", path, S2A_TRAP_MAX);
            m->traps = calloc(n ? n : 1, sizeof *m->traps); m->ntrap = 0; trap_cap = (unsigned)n; have_traps = 1;
        }
        else if (!strcmp(key, "TRAP")) {   // # TRAP <偏移> <种类> <寄存器> <原字>
            char kind[16]; unsigned reg; unsigned long orig; static const char *const kinds[] = {"tpidr_r", "tpidrro_r", "tpidr_w", "ctr"};
            if (!have_traps || m->ntrap >= trap_cap || sscanf(line, "# TRAP %lx %15s %u %lx", &v, kind, &reg, &orig) != 4 || reg > 31 || (v & 3))
                die("%s: TRAP 行格式错或多于 TRAP_SITES", path);
            int k = -1; for (int j = 0; j < 4; j++) if (!strcmp(kind, kinds[j])) k = j;
            if (k < 0) die("%s: TRAP 种类 %s 不认", path, kind);
            m->traps[m->ntrap++] = (struct s2a_trap){.off = v, .orig = (uint32_t)orig, .kind = (uint8_t)k, .reg = (uint8_t)reg};
        }
        else if (!strcmp(key, "FAR_JUMPS")) {   // 随后 n 行 # FAR <从> <到> <E|X>
            unsigned long n = strtoul(line + 2 + strlen(key), NULL, 10);
            if (m->fars || n > (1u << 22)) die("%s: FAR_JUMPS 重复或过多", path);
            m->fars = calloc(n ? n : 1, sizeof *m->fars); m->nfar = 0; far_cap = (unsigned)n; have_fars = 1;
        }
        else if (!strcmp(key, "FAR")) {
            char a[32], b[32], e; 
            if (!have_fars || m->nfar >= far_cap || sscanf(line, "# FAR %31s %31s %c", a, b, &e) != 3 || (e != 'E' && e != 'X')) die("%s: FAR 行格式错或多于 FAR_JUMPS", path);
            int64_t from = strtoll(a, NULL, 16), to = strtoll(b, NULL, 16);
            if ((from | to) & 3) die("%s: FAR %s → %s 不是 4 字节对齐", path, a, b);
            m->fars[m->nfar++] = (struct s2a_far){.from = from, .to = to, .entry = e == 'E'};
        }
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "LOAD_BIAS")) m->load_bias = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else for (int k = 0; k < KEY_N; k++) {
            char want[32]; snprintf(want, sizeof want, "KEY_%s_IDX", k_keyname[k]);
            if (!strcmp(key, want)) { m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k; }
        }
    }
    // The caller owns the file or APK asset lease.
#if defined(__ANDROID__)
    if (!android_target) die("%s: 缺 TARGET android，不能执行苹果线程槽代码", path);
    if (!android_abi) die("%s: 缺 REWRITE_ABI android-sp-safe-v1", path);
#endif
#if defined(__ANDROID__)
    if (m->load_bias) die("%s: Android mapper does not support nonzero LOAD_BIAS", path);
#endif
    if (!m->patch_off || (m->key_page_off + m->load_bias) % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1 || m->load_bias % 0x1000 || m->load_bias >= 0x4000)   // 键页按宿主页网格（基址前移 load_bias）对齐
        die("%s: 表头不全（PATCH_OFF / 键页 / 四把键），用 runtime/rewriter 重出", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: 补丁区在首址前，但 IMAGE_SIZE / FILE_IMAGE_LEN 缺失或不对", path);
    // 双补丁岛：另一个岛与原布局的岛分处基址两侧（近布局 → 基址前，FAR → 键页后），按宿主页网格对齐
    if (m->patch2_off && ((m->patch2_off < 0) == (m->patch_off < 0) || (m->patch2_off + (int64_t)m->load_bias) % 0x4000 || m->patch2_len % 4
                          || (m->patch2_off > 0 && (uint64_t)m->patch2_off < m->image_size)))
        die("%s: PATCH2_OFF %#llx / PATCH2_LEN %#llx 不对", path, (long long)m->patch2_off, (unsigned long long)m->patch2_len);
    if (have_traps && m->ntrap != trap_cap) die("%s: TRAP 行 %u 条，表头 TRAP_SITES %u", path, m->ntrap, trap_cap);
    if (have_fars && m->nfar != far_cap) die("%s: FAR 行 %u 条，表头 FAR_JUMPS %u", path, m->nfar, far_cap);
    if (m->nfar) {   // 远跳按 from 升序（traps.c 二分查）；入口在代码段，其余在补丁岛（陷入时按 pc 所在的段核）
        qsort(m->fars, m->nfar, sizeof *m->fars, far_cmp);
        for (unsigned i = 0; i < m->nfar; i++) {
            const struct s2a_far *f = &m->fars[i];
            if (i && f->from == m->fars[i - 1].from) die("%s: FAR 起点 %#llx 重复", path, (long long)f->from);
            if (f->entry && (f->from < 0 || (uint64_t)f->from >= m->text_end)) die("%s: FAR 入口 %#llx 不在代码段", path, (long long)f->from);
        }
    }
    for (unsigned i = 0; i < m->ntrap; i++) if (m->traps[i].off >= m->text_end) die("%s: TRAP 点 %#llx 不在代码段", path, (unsigned long long)m->traps[i].off);
}
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r"); if (!f) die("打不开 %s", path);
    g_boot_file = f;
    struct loader_text text = {.file = f, .what = path};
    read_layout_text(m, &text);
    fclose(f); g_boot_file = NULL;
}
#if !defined(__ANDROID__)
int s2a_key_page_writable(module_t *m) {
    if (!m || !m->base || !m->key_page_size) return -1;
    return mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ | PROT_WRITE);
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > (m->patch_off < 0 ? m->image_size : m->size)) die("%s: 键页越界", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < KEY_N; i++) {
        uint64_t off;
        if (plat_tsd_offset((uint32_t)key_value(i), &off)) die("%s: 取 %s 的 TSD 偏移失败", m->name, k_keyname[i]);
        page[m->key_idx[i]] = off;
    }
    for (unsigned i = 0; i < KEY_N; i++) if (!page[m->key_idx[i]]) die("%s: 键页 %s 为 0（会读到 TSD 0 号槽）", m->name, k_keyname[i]);
    if (mprotect(page, m->key_page_size, PROT_READ)) die("%s 键页 mprotect", m->name);
}
#endif
static void register_gates_text(module_t *m, struct loader_text *text) {
    char line[256]; int ng = 0;
    unsigned long ret, orig; char kind[16]; int arg;
    while (text_line(text, line, sizeof line)) {
        int count = sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig);
#if defined(__ANDROID__)
        char tail; int used = 0;
        if (sscanf(line, "%lx %15s %d %lx %n%c", &ret, kind, &arg, &orig, &used, &tail) != 4)
            die("%s: 门表行格式错误", text->what);
        if (!s2a_in_patch(m, (uint64_t)m->base + ret) || orig >= m->text_end || (ret & 3) || (orig & 3))   // 返回点在任一补丁岛里
            die("%s: 门返回点或原址越界", text->what);
#endif
        if (count == 4) {
            gate_register((uint64_t)m->base + ret, kind, arg, (uint64_t)m->base + orig);
            ng++;
        }
    }
    LOG("%s: 门 %d 个，PATCH_OFF %#llx", m->name, ng, (long long)m->patch_off);
    if (m->patch2_off) LOG("%s: 双补丁岛 PATCH2_OFF %s%#llx PATCH2_LEN %#llx，brk 兜底 %u 处", m->name, m->patch2_off < 0 ? "-" : "",
                           (unsigned long long)(m->patch2_off < 0 ? -m->patch2_off : m->patch2_off), (unsigned long long)m->patch2_len, m->ntrap);
}
static void register_gates(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r"); if (!f) die("打不开 %s", path);
    g_boot_file = f;
    struct loader_text text = {.file = f, .what = path};
    register_gates_text(m, &text);
    fclose(f); g_boot_file = NULL;
}

// ---------- 重定位 ----------
// DT_RELR：偶数字 = 槽地址（改它并把游标移到下一字），奇数字 = 位图（第 i 位 = 游标后第 i−1 字也是槽），处理完游标前移 63 字
static unsigned relr_apply(module_t *m, uint64_t delta) {
    uint64_t relr = 0, relrsz = 0; unsigned n = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) { if (d->d_tag == DT_RELR) relr = d->d_un.d_ptr; if (d->d_tag == DT_RELRSZ) relrsz = d->d_un.d_val; }
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        if (!(e[i] & 1)) { where = (uint64_t *)(m->base + e[i]); *where++ += delta; n++; }
        else { if (!where) die("%s: DT_RELR 位图项前没有起点", m->name); for (int b = 1; b < 64; b++) if (e[i] >> b & 1) { where[b - 1] += delta; n++; } where += 63; }
    }
    return n;
}
static unsigned g_unresolved, g_deferred;
// 起局没解析的导入位置（绑在 unresolved_called 桩上）：nn::ro 装进 NRO 时，新模块定义了的名字改指到 NRO 里（s2a_nro_map → nro_rebind）。
//   真机 rtld 把这类导入留给延迟绑定，首次调用时再查（含手动装载模块）；我方起局就绑死报错桩，NRO 装上后不补绑就照旧中止
//   （10-29 忍者龟：main 的 EOS_* 导入由 romfs/nro/EOSSDK-Switch-Shipping.nro 定义，装上 NRO 后 main+0x689e1c 仍进报错桩）
typedef struct { uint64_t *where; const char *name; int64_t addend; uint8_t slot; } pend_t;
static pend_t *g_pend; static size_t g_npend, g_cpend;
// AAELF64 动态表：R_AARCH64_JUMP_SLOT 的运算是 S + A。同函数里 ABS64 / GLOB_DAT / RELATIVE 都加了 r_addend，
// 这里原先只存 S。AAELF64 又写明：RELA 的 JUMP_SLOT 加数由平台 ABI 决定要不要认（可以出于安全不认）。
// 缺省不认，和改前逐字节相同；S2A_JUMP_SLOT_ADDEND=1 才按 S + A。加数为 0 时两种结果相同。
static int jump_slot_addend(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("S2A_JUMP_SLOT_ADDEND"); v = e && strcmp(e, "1") == 0; }
    return v;
}
void s2a_nro_table_load(const char *dir); const char *s2a_nro_defer_lookup(const char *name);   // nrodefer.c：romfs NRO 导出表
uint64_t s2a_direct_lookup(const char *name);
#if !defined(__ANDROID__)
uint64_t s2a_direct_stub(const char *name);   // gfxmode.c：找遍之后，直连模式下 gl* 核心名的安全空实现
#endif
static uint64_t resolve_global(const char *name) {   // 按装载顺序找已装模块里的定义（ELF 缺省语义 = Horizon rtld）
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return 0;
}
// Horizon 装载序校验（tools/module_order.py / BootContract.java 同一份规则）：main → subsdk0..N（数字序）→ sdk；
// 名单外的名排在 subsdk* 与 sdk 之间、按名定序（宿主自己扮演 rtld，guest rtld 从不装载）。
// 顺序错 = 上面 resolve_global 把 subsdk 与 sdk 重名的定义绑反，报红拒绝起局，不放行（11-06 安卓字母序包事故）。
static int subsdk_index(const char *n) {
    if (strncmp(n, "subsdk", 6) || !n[6]) return -1;
    long v = 0;
    for (const char *p = n + 6; *p; p++) {
        if (*p < '0' || *p > '9' || (v = v * 10 + (*p - '0')) > 0x3fffffff) return -1;
    }
    return (int)v;
}
static int order_group(const char *n) { return !strcmp(n, "main") ? 0 : subsdk_index(n) >= 0 ? 1 : !strcmp(n, "sdk") ? 3 : 2; }
static int order_cmp(const char *a, const char *b) {
    int g = order_group(a) - order_group(b);
    if (g) return g;
    if (order_group(a) == 1) return subsdk_index(a) - subsdk_index(b);
    if (order_group(a) == 2) return strcmp(a, b);
    return 0;
}
int s2a_module_order_ok(const char *const *names, int n) {
    if (n <= 0) return 1;
    if (strcmp(names[0], "main")) return 0;
    for (int i = 1; i < n; i++) if (order_cmp(names[i - 1], names[i]) > 0) return 0;
    return 1;
}
// 直连模式下游戏对 nn::ro::LookupSymbol(uintptr_t *out, const char *name) 的导入绑到下面的跳板（宿主自己的代码，不要可写可执行页）：
//   先只问直连图形层（s2a_direct_lookup：直连不装 NVIDIA Vulkan / GL 驱动模块，游戏按名取 vk*——Minecraft 运行时 LookupSymbol("vkGetInstanceProcAddr")、
//   main 不导入任何 vk*——要拿到宿主图形层，与同名导入解析同一个地址）；命中写 *out、回 0。不命中恢复 x0 / x1 / x30，尾跳 sdk 自己的 LookupSymbol
//   （装载时 resolve_global 取到的定义：它遍历 rtld 交给 nn::ro 的模块链，含运行时 nn::ro::LoadModule 装的 NRO——那条链归装载器扮演 rtld 的一侧填）；
//   sdk 没定义时退到 ro_not_found，回 sdk 原实现找不到时的结果码 0xA16（Minecraft 1.21.111 sdk 反汇编）。
//   通用模式完全不挂：导入照旧解析到 sdk 原定义，驱动模块照装。只接 LookupSymbol：LookupModuleSymbol 只查它那个 NRO，图形层不在其中。
#define RO_LOOKUP_SYMBOL "_ZN2nn2ro12LookupSymbolEPmPKc"
#define RO_RESULT_NOT_FOUND 0xA16u
#if defined(__aarch64__)
__attribute__((visibility("hidden"))) uint64_t s2a_ro_direct_fn = 0, s2a_ro_sdk_fn = 0;   // 跳板读：宿主查询（已套切栈桩）、sdk 原定义（或 ro_not_found）
void s2a_ro_lookup_tramp(void);
#if defined(__APPLE__)
#define RO_S(x) "_" #x
#define RO_PG(x) "_" #x "@PAGE"
#define RO_LO(x) "_" #x "@PAGEOFF"
#define RO_VIS ".private_extern"
#else
#define RO_S(x) #x
#define RO_PG(x) #x
#define RO_LO(x) ":lo12:" #x
#define RO_VIS ".hidden"
#endif
__asm__(".text\n.p2align 2\n.globl " RO_S(s2a_ro_lookup_tramp) "\n" RO_VIS " " RO_S(s2a_ro_lookup_tramp) "\n" RO_S(s2a_ro_lookup_tramp) ":\n"
        "    stp x29, x30, [sp, #-32]!\n    mov x29, sp\n    stp x0, x1, [sp, #16]\n"   // x0 = out，x1 = name；不碰 x18
        "    mov x0, x1\n    adrp x16, " RO_PG(s2a_ro_direct_fn) "\n    ldr x16, [x16, " RO_LO(s2a_ro_direct_fn) "]\n    blr x16\n"
        "    cbz x0, 1f\n    ldr x1, [sp, #16]\n    str x0, [x1]\n    mov w0, #0\n    ldp x29, x30, [sp], #32\n    ret\n"
        "1:  ldp x0, x1, [sp, #16]\n    ldp x29, x30, [sp], #32\n"
        "    adrp x16, " RO_PG(s2a_ro_sdk_fn) "\n    ldr x16, [x16, " RO_LO(s2a_ro_sdk_fn) "]\n    br x16\n");
static uint64_t ro_direct_lookup(const char *name) { return name ? s2a_direct_lookup(name) : 0; }
static uint32_t ro_not_found(uint64_t *out, const char *name) { (void)out; (void)name; return RO_RESULT_NOT_FOUND; }
static uint64_t ro_lookup_bind(const char *who) {   // 直连模式下 LookupSymbol 导入的绑定值：跳板（两个槽在装载期重定位时填，游戏代码跑起来之后只读）
    uint64_t sdk = resolve_global(RO_LOOKUP_SYMBOL);
    s2a_ro_sdk_fn = sdk ? sdk : (uint64_t)ro_not_found;
    s2a_ro_direct_fn = (uint64_t)sw_xstack_wrap("ro_direct_lookup", (void *)ro_direct_lookup);
    LOG("直连：%s 的 nn::ro::LookupSymbol 导入接宿主跳板（先问直连图形层，不中走%s）", who, sdk ? " sdk 原定义" : "「回 0xA16」：没有模块定义它");
    return (uint64_t)s2a_ro_lookup_tramp;
}
#endif
// 未解析的强导入：装载期报红记名；运行时若真被调到，进这里再报一次（调用点 = lr）
__attribute__((noreturn)) static void unresolved_called(void) {
    uint64_t lr = (uint64_t)__builtin_return_address(0); module_t *m = module_of((void *)lr);
    die("【红】游戏调用了装载期未解析的导入（调用点 %s+%#llx，名字见装载期【红】行或「延迟到 nn::ro 装载」行：游戏调 nn::ro 装 NRO 还没实现）", m ? m->name : "?", (unsigned long long)(m ? lr - (uint64_t)m->base : lr));
}
// 调试门（S2A_RELOC_DEBUG=1）：本模块有定义的符号却绑到先装的模块 = 装载顺序错的指纹，打条数与前几个样例名
static int reloc_foreign_debug(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("S2A_RELOC_DEBUG"); v = e && strcmp(e, "1") == 0; }
    return v;
}
static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case 3 /* DT_PLTGOT */: m->pltgot = d->d_un.d_ptr; break;
    }
    unsigned cnt[3] = {0}, nlibc = 0, nweak0 = 0, ntls = 0, nforeign = 0;
    const int rdbg = reloc_foreign_debug(); char foreign[160] = "";
    cnt[0] += relr_apply(m, (uint64_t)m->base);
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            // 同游戏 SDK nn::ro::detail::RoModule::Relocation：JUMP_SLOT 槽的原值（链接时填的 PLT 表头）+ 基址 = 懒绑定入口，
            //   ro_handoff 写进本模块的 RoModule（游戏的 nn::ro 卸 NRO 时 RevertReference 把指向它的函数槽改回这个入口）
            if (type == R_AARCH64_JUMP_SLOT && !m->plt_stub && *where) m->plt_stub = (uint64_t)m->base + *where;
            enum reloc_class rc = reloc_class(type);
            if (rc == RELOC_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            if (rc == RELOC_TLS_LEFT) { ntls++; continue; }   // 同任天堂 rtld：TLS 重定位留给游戏 sdk 的 __nnmusl_init_dso（reloc_class.h）
            if (rc == RELOC_UNKNOWN) die("%s: 不认识的重定位类型 %u", m->name, type);
            const Elf64_Sym *s = &m->symtab[si]; const char *nm = m->strtab + s->st_name; uint64_t v = 0;
            if (s->st_shndx) {   // 本模块有定义：局部绑定 / 非默认可见性绑自己，否则按装载顺序全局查（先装模块的定义胜出）
                uint64_t self = (uint64_t)m->base + s->st_value;
                v = (s->st_info >> 4) == 0 || (s->st_other & 3) ? self : resolve_global(nm);
                if (!v) v = self;
                if (rdbg && v != self) {   // 本模块有定义却绑到先装的别的模块：装载顺序错时的指纹（默认可见性才全局查）
                    nforeign++;
                    if (nforeign <= 4) snprintf(foreign + strlen(foreign), sizeof foreign - strlen(foreign), "%s%s", nforeign > 1 ? " " : "", nm);
                }
            } else {
#if defined(__aarch64__)
                if (!strcmp(nm, RO_LOOKUP_SYMBOL) && s2a_gfx_mode() == S2A_GFX_DIRECT) v = ro_lookup_bind(m->name);   // 按名查符号：直连先问图形层（上面的跳板）
                else
#endif
                v = s2a_direct_lookup(nm);   // 直连模式（gfxmode.c）：游戏导入的 nvnBootstrapLoader 解析到直连图形层；通用模式恒 0，走下面原路径
                if (!v) v = resolve_global(nm);
                if (!v) { void *f = s2a_libc_lookup(nm); if (f) { v = (uint64_t)sw_xstack_wrap(nm, f); nlibc++; } }
#if !defined(__ANDROID__)   // 安卓没有直连 GL 层（s2a_direct_lookup 在 android/native 自给）
                if (!v && !s2a_nro_defer_lookup(nm)) v = s2a_direct_stub(nm);   // 都没定义（romfs NRO 也没有）的 gl* 核心名：安全空实现，放最后免得顶掉游戏模块自己的定义（#601）
#endif
                if (!v && (s->st_info >> 4) == 2) { nweak0++; *where = type == R_AARCH64_JUMP_SLOT ? 0 : (uint64_t)r->r_addend; cnt[type == R_AARCH64_JUMP_SLOT ? 2 : 1]++; continue; }   // 无定义弱符号 = 0（ELF / rtld 语义）
                if (!v) {   // 游戏自带 NRO（romfs/nro）的导出：Horizon 上要等 nn::ro 装进来才解析，记普通日志不计红；桩不变，真被调到照旧报红中止
                    const char *nro = s2a_nro_defer_lookup(nm);
                    if (nro) { g_deferred++; LOG("%s: 导入 %s 延迟到 nn::ro 装载（romfs/nro/%s 定义）", m->name, nm, nro); }
                    else { g_unresolved++; LOG("【红】%s: 导入 %s 没有任何模块定义", m->name, nm); }
                    v = (uint64_t)unresolved_called;
                    if (g_npend == g_cpend) { size_t c = g_cpend ? g_cpend * 2 : 256; pend_t *n = realloc(g_pend, c * sizeof *n); if (!n) die("未解析导入表扩到 %zu 项时内存不足", c); g_pend = n; g_cpend = c; }
                    g_pend[g_npend++] = (pend_t){where, nm, r->r_addend, type == R_AARCH64_JUMP_SLOT}; }
            }
#ifdef __ANDROID__
            uint64_t fb = v;   // Android does not provide the far-window binding.
#else
            uint64_t fb = type == R_AARCH64_JUMP_SLOT ? s2a_far_bind(nm, v) : v;   // 远端开着时 SDK 内存函数族换成远端感知版（far.c）
#endif
            if (type == R_AARCH64_JUMP_SLOT && !jump_slot_addend()) { *where = fb; cnt[2]++; }   // 缺省：只存 far_bind(S)（改前）
            else if (type == R_AARCH64_JUMP_SLOT) {   // S2A_JUMP_SLOT_ADDEND=1：S + A；换了桩时桩没有 +A 这个入口，加数非 0 就不换桩、报红（#743）
                if (fb == v || !r->r_addend) *where = fb == v ? v + r->r_addend : fb;
                else { *where = v + r->r_addend; LOG("【红】%s: 导入 %s 加数 %lld 非 0，远端桩不接加数，保留 S + A 不换桩", m->name, nm, (long long)r->r_addend); }
                cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }
        }
    LOG("%s: 重定位 relative=%u abs/glob=%u jump=%u（libc 直通 %u，无定义弱符号置 0 %u）", m->name, cnt[0], cnt[1], cnt[2], nlibc, nweak0);
    if (rdbg && nforeign) LOG("%s: %u 处本模块已定义的符号绑到了先装的模块（样例：%s）", m->name, nforeign, foreign);
    if (ntls) LOG("%s: TLS 重定位 %u 条留给游戏 sdk 的 __nnmusl_init_dso（同任天堂 rtld）", m->name, ntls);
}
// 出包期 GLSLC 工具模式（glslc_tool.c）：把模块 m 对 name 的未定义导入改指到 v（JUMP_SLOT 写 v，GLOB_DAT / ABS64 写 v + 加数），返回改了几处。
//   数据段装完一直可写（只有键页另设保护），不用改页权限
int s2a_module_rebind(module_t *m, const char *name, uint64_t v) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0; int n = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
    }
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i]; unsigned type = ELF64_R_TYPE(r->r_info);
            if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) continue;
            const Elf64_Sym *sy = &m->symtab[ELF64_R_SYM(r->r_info)];
            if (sy->st_shndx || strcmp(m->strtab + sy->st_name, name)) continue;
            *(uint64_t *)(m->base + r->r_offset) = type == R_AARCH64_JUMP_SLOT ? v : v + (uint64_t)r->r_addend; n++;
        }
    return n;
}
#ifdef S2A_UNIT_TEST
void s2a_loader_test_relocate(module_t *m) {   // 单测：内存里的合成模块，不走映射
    if (m->symh) { free(m->symh); m->symh = NULL; m->symh_mask = 0; }
    build_symhash(m);
    relocate(m);
}
#endif

#if !defined(__ANDROID__)
// ---------- 模块在窗口里的位置（10-25 马力欧银河 2：main 映像 2.86 GB、bss 约 2.6 GB）----------
// 模块原先在窗口之外另 mmap（落低区）；iPhone 地址约束下窗口外低区最大一块只有约 723 MB，装不下大 bss 的款（Mac 开约束时 FATAL main mmap 0xaa5cc000，iPhone 同样起不来）。
// 现在窗口跨度 = Horizon 各区 + 本函数算的模块跨度；模块段落点由布局定（layout.c），不再另找低区。
// 每个模块占「补丁区（FAR 在基址前）+ 映像」，按 2 MB 对齐排；大小只取包里 <名>.fixups.tsv 表头（PATCH_OFF / IMAGE_SIZE），
// 非 FAR 的补丁区长度 Mac 取 <名>.patched.bin 文件长、iOS 取外置镜像包里 x1 块的大小（偏移表）——与 map_image 算 size 的同一口径
#if TARGET_OS_IPHONE
#include "imgpack.h"     // 外置镜像包：启动上下文的 images（单款兼容 = <App>/Frameworks/S2AImages.dylib），按需 MAP_FIXED 映射
#include "s2a_launch_ctx.h"
// 游戏模块镜像（main / sdk / subsdk* / nn::ro 装的 NRO）全部在外置镜像包里，主程序不含任何游戏镜像（11-04 阶段 0 全外置，维护者 定为唯一路径：
//   主程序 = 与游戏无关的预编壳，pack/shell_build.sh；镜像包由 ios/embed.py 纯 Python 写 Mach-O）。一块 = 包 + 文件偏移 + 长度
typedef struct { const s2a_imgpack *pk; uint64_t off; unsigned long len; } blk_t;
enum { EMB_X0 = S2A_IMG_X0, EMB_W = S2A_IMG_W, EMB_X1 = S2A_IMG_X1, EMB_X2 = S2A_IMG_X2 };
static const s2a_imgpack *ext_pack(void) {   // 进程内打开一次；包不在、打不开或签名不过 = 停（iOS 包必有它）
    static const s2a_imgpack *pk; static int done; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!done) {
        const s2a_launch_ctx *lc = s2a_launch_ctx_get(); char why[512] = "";
        if (!lc) die("外置镜像包：启动上下文不可用：%s", s2a_launch_ctx_error());
        const char *path = lc->images;   // 启动上下文冻结的镜像包（单款兼容 = <可执行文件目录>/Frameworks/S2AImages.dylib，同改造前）
        s2a_imgpack *q = NULL; int rc = s2a_imgpack_open(path, &q, why, sizeof why);
        if (rc) die("外置镜像包打不开（返回 %d）：%s", rc, why);
        pk = q; done = 1;
        LOG("外置镜像包：%s 已打开、签名已注册（镜像按需映射，不占宿主低区）", path);
    }
    pthread_mutex_unlock(&mu); return pk;
}
static char g_img_ns[8];   // 多程序合集：序号 n ≥ 1 的模块在镜像包里叫 p<n>/<名>（序号 0 不带前缀，与单程序包一致）
static s2a_imgpack *g_span_pk;   // 非空：算模块跨度时按这个包的偏移表取块长（多款容器构造期逐款设，s2a_loader_span_images），不碰启动上下文的镜像包
static int blk_get(const char *name, int k, blk_t *b) {   // 找到这个镜像回 1（*b 填来源与长度，长度 0 = 空块），没有回 0
    const s2a_imgpack *pk = g_span_pk ? g_span_pk : ext_pack(); uint64_t off, len; char full[40];
    if (snprintf(full, sizeof full, "%s%s", g_img_ns, name) >= (int)sizeof full) return 0;
    if (s2a_imgpack_find(pk, full, k, &off, &len)) { *b = (blk_t){pk, off, (unsigned long)len}; return 1; }
    return 0;
}
#endif
// 选外置镜像包里哪一支程序的模块（多程序合集，runtime/include/s2a_programs.h）：只 iOS 有镜像包，别的平台空操作
void s2a_loader_image_program(int n) {
#if TARGET_OS_IPHONE
    if (n > 0) snprintf(g_img_ns, sizeof g_img_ns, "p%d/", n); else g_img_ns[0] = 0;
#else
    (void)n;
#endif
}
// 算模块跨度（s2a_module_span）时用哪个外置镜像包（多款容器构造期，runtime/src/vawin.c 逐款调）：path 非空 = 只读这个包的偏移表（imgpack.h s2a_imgpack_index，
//   不注册签名、不冻结启动上下文），NULL = 释放、回到启动上下文的镜像包。成功回 0；包打不开 / 格式不对回非 0、why 写原因。
//   Mac 的非 FAR 补丁区长度取模块目录的 <名>.patched.bin（见 mod_footprint），没有镜像包：空操作回 0
int s2a_loader_span_images(const char *path, char *why, size_t whyn) {
#if TARGET_OS_IPHONE
    s2a_imgpack_free(g_span_pk); g_span_pk = NULL;
    return path ? s2a_imgpack_index(path, &g_span_pk, why, whyn) : 0;
#else
    (void)path; (void)why; (void)whyn; return 0;
#endif
}
#define MOD_ALIGN (2ull << 20)
static uint64_t mod_footprint(const char *dir, const char *name) {
    char path[1100]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, name);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[256], key[32]; long long po = 0, po2 = 0; unsigned long img = 0, bias = 0, l2 = 0, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) continue;
        if (!strcmp(key, "PATCH_OFF")) po = strtoll(line + 2 + strlen(key), NULL, 16);
        else if (!strcmp(key, "PATCH2_OFF")) po2 = strtoll(line + 2 + strlen(key), NULL, 16);
        else if (!strcmp(key, "PATCH2_LEN")) l2 = v;
        else if (!strcmp(key, "IMAGE_SIZE")) img = v;
        else if (!strcmp(key, "LOAD_BIAS")) bias = v;
    }
    fclose(f);
    uint64_t n, up2 = (l2 + 0x3FFF) & ~0x3FFFull;
    if (po < 0) n = (uint64_t)-po + (po2 > 0 ? (uint64_t)po2 + up2 : img);   // FAR：补丁区在基址前 + 映像（含 bss、键页）；双岛再到键页后的后岛末
    else {
#if TARGET_OS_IPHONE
        blk_t b1;
        if (!blk_get(name, EMB_X1, &b1)) return 0;
        n = (uint64_t)po + b1.len;
#else
        snprintf(path, sizeof path, "%s/%s.patched.bin", dir, name); struct stat st;
        if (stat(path, &st) || (uint64_t)st.st_size < l2) return 0;
        n = ((uint64_t)st.st_size - l2 + 0x3FFF) & ~0x3FFFull;   // 双岛：文件末尾 PATCH2_LEN 字节是基址前的前岛
#endif
        n += bias;   // 映射从基址前 bias 字节起（见 read_layout LOAD_BIAS）
        if (po2 < 0) n += (uint64_t)-po2 - bias;   // 双岛的前岛：映射起点再往前 FRONT
    }
    return (n + MOD_ALIGN - 1) & ~(MOD_ALIGN - 1);
}
uint64_t s2a_module_span(const char *dir) {   // 模块目录里全部 <名>.fixups.tsv 对应模块的跨度和（vawin.c 构造期占窗、svc_mem.c 收窗都用它）
    DIR *d = opendir(dir); if (!d) return 0;
    uint64_t t = 0; struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name); const char *suf = ".fixups.tsv"; size_t sl = strlen(suf);
        if (l <= sl || strcmp(e->d_name + l - sl, suf)) continue;
        char nm[64]; if (l - sl >= sizeof nm) continue; memcpy(nm, e->d_name, l - sl); nm[l - sl] = 0;
        t += mod_footprint(dir, nm);
    }
    closedir(d); return t;
}
static uint64_t g_modcur;   // 下一个模块的落点（窗口里模块段的游标）；0 = 没有窗口（S2A_VAWIN_NORESERVE 诊断），照旧由系统挑地址
static uint64_t g_slot_at;  // 非 0：下一次 mod_slot 原地复用这个落点（nn::ro 卸了再装同一个 NRO：映回上次的地址，不再从游标切）
// 模块落点：窗口里有模块段就从游标切一块（2 MB 对齐），否则 0（调用方用 ANYWHERE）
static uint64_t mod_slot(module_t *m, uint64_t len) {
    if (g_slot_at) { uint64_t at = g_slot_at; g_slot_at = 0; return at; }
    if (!g_modcur) return 0;
    uint64_t at = g_modcur; g_modcur += (len + MOD_ALIGN - 1) & ~(MOD_ALIGN - 1);
    uint64_t wb, ws; s2a_window_get(&wb, &ws);
    if (g_modcur > wb + ws) die("%s 映像 %#llx 字节放不进窗口里的模块段（窗口 %#llx +%#llx，游标已到 %#llx）", m->name, (unsigned long long)len, (unsigned long long)wb, (unsigned long long)ws, (unsigned long long)g_modcur);
    return at;
}

#else
uint64_t s2a_module_span(const char *dir) { (void)dir; return 0; }
#endif

// ---------- 映射 ----------
#if defined(__ANDROID__)
// 目录与 APK 输入共用 mapper、表头一致性检查和进程期映射所有权。
static void adopt_mapping(module_t *m, struct s2a_module *mapped) {
    const struct s2a_module_layout *l = &mapped->l;
    if (l->patch_off != m->patch_off || l->key_page_off != m->key_page_off ||
        l->key_page_size != m->key_page_size || l->text_end != m->text_end ||
        l->image_size != m->image_size || l->file_image_len != m->file_image_len ||
        l->patch2_off != m->patch2_off || l->patch2_len != m->patch2_len ||
        memcmp(l->key_idx, m->key_idx, sizeof m->key_idx)) {
        s2a_module_unmap(mapped);
        die("%s: 装载期间映射表头发生变化", m->name);
    }
    m->base = mapped->base;
    m->size = mapped->map_size - (size_t)(mapped->base - mapped->map);
    m->patch_len = mapped->patch_size;
    // 映射归进程期模块表；失败不能清掉正被 guest/服务使用的窗口。
}
static void mapping_offsets(module_t *m, uint64_t offsets[KEY_N]) {
    for (unsigned i = 0; i < KEY_N; i++)
        if (plat_tsd_offset((uint32_t)key_value(i), &offsets[i]) || !offsets[i])
            die("%s: %s 的实际 TSD 偏移不可用", m->name, k_keyname[i]);
}
static void map_image(module_t *m, const char *dir) {
    uint64_t offsets[KEY_N]; mapping_offsets(m, offsets);
    struct s2a_module mapped; char err[512];
    int rc = s2a_module_map(dir, m->name, offsets, &mapped, err, sizeof err);
    if (rc) die("%s: Android 映射失败 rc=%d %s", m->name, rc, err);
    adopt_mapping(m, &mapped);
}

#elif TARGET_OS_IPHONE
// iOS：映像在外置镜像包 Frameworks/S2AImages.dylib（imgpack.c；代码块 x0 / x1 / x2 与 W 块，位置查包里的偏移表）。保留一块连续地址，按原相对位置
//   文件 mmap 回去（MAP_FIXED | MAP_PRIVATE：代码 R|X 且最大权限压到 R|X，W 写时复制）；主程序不含游戏镜像，不占宿主低区
static void map_blk(module_t *m, vm_address_t to, const blk_t *b, unsigned long skip, int code) {   // 块 b 从第 skip 字节起映射到 to
    int e = 0, rc = s2a_imgpack_map(b->pk, (void *)to, b->off + skip, b->len - skip, code, &e);
    if (rc) die("%s 外置镜像映射 %#lx（文件偏移 %#llx，%#lx 字节，%s）失败：返回 %d errno=%d", m->name, (unsigned long)to, (unsigned long long)(b->off + skip), b->len - skip, code ? "代码" : "可写", rc, e);
}
// 进程内重启（11-02 测试设备2 塞尔达切 60 帧「立刻重启」：重建窗口 EACCES）：上一局映射进窗口的代码页系统不许覆盖或解除（外置的文件代码页
//   11-03 测试设备2 实测 munmap 后留「权限 0 / 最大 0」占位、再映射 EACCES；11-04 测试设备2 全外置泰拉瑞亚实测重启后复用、继续出帧），vawin.c s2a_window_session_reset 把它们原样留着。
//   新局同一款、同一落点（模块游标每局复位）代码页内容不变，认出来直接复用，同 nn::ro 重装 remap_writable：
//   只把落点里代码页以外的部分重新清零可写、W 换回干净副本、键页重装。落点上是别的模块的代码（运行期 nn::ro 装 NRO 的先后变了）就换下一个落点
static int span_has_signed_code(vm_address_t lo, vm_address_t hi) {   // [lo, hi) 里有没有代码页（判据 imgpack.h s2a_prot_is_code：当前权限含执行，或最大权限可执行不可写）
    return s2a_span_has_code(lo, hi);
}
// [at, at + z) 整段已映射、可读，且与块 b 第 skip 字节起的内容逐字节相同（调用方跳过 x0 前面 load_bias 个零字节）。比整块（审计 P1-84：原先只比前 16 KB，
//   两个内嵌代码块前 16 KB 相同、后面不同时会把别人的后半段当自己的接着执行）；只在进程内重启的复用路径上跑，几十 MB 的比较是毫秒级
static int code_same(vm_address_t at, const blk_t *b, unsigned long skip) {
    unsigned long z = b->len - skip;
    if (!z) return 0;
    return s2a_imgpack_same(b->pk, (const void *)at, b->off + skip, z);   // 逐段核未映射 / 不可读的段在 imgpack.c 里做
}
static void zero_except(module_t *m, vm_address_t lo, vm_address_t hi, vm_address_t c[][2], int nc) {   // [lo, hi) 里 c[] 以外的部分换成新的零页（可读写，同首次 vm_allocate）
    for (vm_address_t a = lo; a < hi;) {
        vm_address_t b = hi;
        for (int i = 0; i < nc; i++) if (c[i][1] > c[i][0] && a >= c[i][0] && a < c[i][1]) { a = c[i][1]; b = 0; break; }
        if (!b) continue;
        for (int i = 0; i < nc; i++) if (c[i][1] > c[i][0] && c[i][0] > a && c[i][0] < b) b = c[i][0];
        vm_address_t z = a; kern_return_t kr = vm_allocate(mach_task_self(), &z, b - a, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
        if (kr != KERN_SUCCESS) die("%s 复用代码页时清零 %#lx +%#lx 失败：%d", m->name, (unsigned long)a, (unsigned long)(b - a), kr);
        a = b;
    }
}
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    blk_t x0, w, x1, x2 = {0};
    if (!blk_get(m->name, EMB_X0, &x0) || !blk_get(m->name, EMB_W, &w) || !blk_get(m->name, EMB_X1, &x1))
        die("模块 %s 的 x0 / W / x1 块缺失（外置镜像包偏移表里没有）", m->name);
    unsigned long z0 = x0.len, zw = w.len, z1 = x1.len, z2 = 0;
    // 双补丁岛：另一个岛是 x2 块（embed.py 取 patched.bin 末尾 PATCH2_LEN 字节，补到 16K）
    int has2 = m->patch2_len && blk_get(m->name, EMB_X2, &x2); z2 = has2 ? x2.len : 0;
    if (m->patch2_len && (!has2 || z2 != ((m->patch2_len + 0x3FFF) & ~0x3FFFull) || (x2.off & 0x3FFF)))
        die("%s 外置镜像包 x2 块缺失或长度 %#lx 与 PATCH2_LEN %#llx 不符", m->name, z2, (unsigned long long)m->patch2_len);
    int front = m->patch_off < 0;
    // 装载偏置：x0 节前面多 load_bias 个零字节（embed.py），整段映射从 base − load_bias 起
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off + m->load_bias) || ((z0 | zw | z1) & 0x3FFF) || ((x0.off | w.off | x1.off) & 0x3FFF) || (front && m->load_bias))
        die("%s 外置镜像包块布局不符（x0 %#lx w %#lx x1 %#lx，PATCH_OFF %#llx）", m->name, z0, zw, z1, (long long)m->patch_off);
    m->size = front ? m->image_size : (uint64_t)m->patch_off + z1;
    uint64_t lb = m->load_bias;
    uint64_t pre = front ? (uint64_t)-m->patch_off : m->patch2_off < 0 ? (uint64_t)-m->patch2_off - lb : 0;   // 基址前（近布局双岛：前岛在映射起点前 FRONT）
    uint64_t span = m->patch2_off > 0 ? (uint64_t)m->patch2_off + z2 : m->size;                                // FAR 双岛：映射到键页后的后岛末（z2 = 后岛 16K 圆整长）
    if (front) m->patch_len = z1;
    vm_address_t base = 0; int reuse = 0;
    for (int tries = 0;; tries++) {
        base = (vm_address_t)mod_slot(m, pre + lb + span);
        if (!base || !span_has_signed_code(base, base + pre + lb + span)) break;
        vm_address_t mb = base + pre + lb;
        if (z0 > lb && code_same(base + pre + lb, &x0, lb) && (!z1 || code_same(mb + m->patch_off, &x1, 0)) && (!z2 || code_same(mb + m->patch2_off, &x2, 0))) { reuse = 1; break; }
        LOG("%s：落点 %#lx 上留着上一局别的模块的代码页（iOS 盖不掉），换下一个落点", m->name, (unsigned long)base);
        if (tries >= 64) die("%s：连续 64 个落点都被上一局的代码页占着", m->name);
    }
    int boot_slot = boot_map_slot(m);
    if (reuse) {
        vm_address_t mb = base + pre + lb, c[3][2] = {{base + pre, base + pre + z0}, {z1 ? mb + m->patch_off : 0, z1 ? mb + m->patch_off + z1 : 0}, {z2 ? mb + m->patch2_off : 0, z2 ? mb + m->patch2_off + z2 : 0}};
        zero_except(m, base, base + pre + lb + span, c, 3);
        static int said; if (!said++) LOG("进程内重启：%s 等模块的代码页原样复用（同一落点、内容相同），只换回 W / 清零其余", m->name);
    } else {
        kern_return_t kra = vm_allocate(mach_task_self(), &base, pre + lb + span, base ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE : VM_FLAGS_ANYWHERE);
        if (kra != KERN_SUCCESS) die("%s 保留 %#llx 字节地址失败：%d", m->name, (unsigned long long)(pre + lb + span), kra);
    }
    boot_map_record(boot_slot, base, pre + lb + span);
    // Keep ownership even if one of the following remaps fails before this
    // module reaches g_nmod. The recorded allocation includes both islands,
    // their gaps and the load bias, rather than reconstructing a single island.
    m->base = (uint8_t *)(base + pre + lb);
    if (!reuse) map_blk(m, base + pre, &x0, 0, 1);   // x0 从 base − load_bias（= 分配起点 + pre）起；复用时代码页已在
    map_blk(m, base + pre + z0, &w, 0, 0);
    base += pre + lb;
    if (z1 && !reuse) map_blk(m, base + m->patch_off, &x1, 0, 1);   // 补丁区可空（改写没有补丁点）
    if (z2 && !reuse) map_blk(m, base + m->patch2_off, &x2, 0, 1);
    m->base = (uint8_t *)base; install_key_page(m);
}
// nn::ro 卸了再装（iOS）：代码页（x0 / x1 / x2）内容不变、盖不掉，原地留着；只把 W 换回包里的干净副本、
//   FAR 的 bss 段重新清零、键页重装。不再整段 vm_allocate 覆盖：11-02 马力欧派对 nro115 第二次装，覆盖已映射的代码页那一步失败
//   （FATAL「保留 0x54000 字节地址失败」）。外置镜像的 W 再映射一次即干净副本（11-03 测试设备2 实测 W 盖 W 可行）
static void remap_writable(module_t *m) {
    blk_t x0, w;
    if (!blk_get(m->name, EMB_X0, &x0) || !blk_get(m->name, EMB_W, &w)) die("模块 %s 的 x0 / W 块缺失（外置镜像包偏移表里没有）", m->name);
    map_blk(m, (vm_address_t)(m->base - m->load_bias) + x0.len, &w, 0, 0);
    if (m->patch_off < 0 && m->image_size > m->file_image_len) {   // FAR：[FILE_IMAGE_LEN, IMAGE_SIZE) 是 bss 与键页，原先是 vm_allocate 的零页
        vm_address_t z = (vm_address_t)m->base + m->file_image_len; kern_return_t kr;
        if ((kr = vm_allocate(mach_task_self(), &z, m->image_size - m->file_image_len, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE)) != KERN_SUCCESS)
            die("%s 重装清零 bss（%#llx 字节）失败：%d", m->name, (unsigned long long)(m->image_size - m->file_image_len), kr);
    }
    install_key_page(m);
}
#else
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY); if (fd < 0) die("打不开 %s", path);
    g_boot_fd = fd;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX)
        die("%s 不是可读取的非空普通映像文件", path);
    uint8_t *p = malloc((size_t)st.st_size); g_boot_raw = p;
    if (!p) die("读取 %s 的临时映像分配失败", path);
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t got = read(fd, p + used, (size_t)st.st_size - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) die("读 %s 不全", path);
        used += (size_t)got;
    }
    close(fd); g_boot_fd = -1; *n = used; return p;
}
// 封代码区：只读+执行，并把最高权限也压到 R|X——此后谁想把游戏代码页改成可写，mprotect 当场 EACCES，与 iOS 签名代码页同行为，
// Mac 上就能拦住「Mac 绿、iOS 崩」的运行时改代码
static void seal_rx(module_t *m, void *a, size_t n, const char *what) {
    if (mprotect(a, n, PROT_READ | PROT_EXEC)) die("%s mprotect %s", m->name, what);
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)a, n, TRUE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) die("%s 压 %s 最高权限失败：%d", m->name, what, kr);
}
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {   // FAR：文件 = 映像前段（text+ro+data）+ 补丁区；bss 与键页为匿名零页；补丁区末到基址的空隙 PROT_NONE
        // 双补丁岛：文件末尾再接 PATCH2_LEN 字节的后岛（键页后 PATCH2_OFF 处），后岛前的空隙 PROT_NONE
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len, l2 = m->patch2_len, up2 = (l2 + 0x3FFF) & ~0x3FFFull;
        if (sz < fl + l2) die("%s 比 FILE_IMAGE_LEN %#llx + PATCH2_LEN %#llx 短", path, (unsigned long long)fl, (unsigned long long)l2);
        m->size = m->image_size; m->patch_len = (sz - l2 - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s 补丁区 %#llx 字节放不进首址前的 %#llx", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint64_t span = m->patch2_off > 0 ? (uint64_t)m->patch2_off + up2 : m->size;
        uint64_t at = mod_slot(m, pre + span);
        int boot_slot = boot_map_slot(m);
        uint8_t *a = mmap((void *)at, pre + span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (at ? MAP_FIXED : 0), -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + span));
        boot_map_record(boot_slot, (uint64_t)a, pre + span);
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - l2 - fl);
        if (l2) memcpy(m->base + m->patch2_off, raw + sz - l2, l2);
        free(raw); g_boot_raw = NULL;
        install_key_page(m);
        seal_rx(m, m->base, (m->text_end + 0x3FFF) & ~0x3FFFull, "text"); if (m->patch_len) seal_rx(m, a, m->patch_len, "patch");
        if (pre > m->patch_len && mprotect(a + m->patch_len, pre - m->patch_len, PROT_NONE)) die("%s mprotect gap", m->name);
        if (m->patch2_off > 0) {
            if (up2) seal_rx(m, m->base + m->patch2_off, up2, "patch2");
            if ((uint64_t)m->patch2_off > m->size && mprotect(m->base + m->size, (uint64_t)m->patch2_off - m->size, PROT_NONE)) die("%s mprotect gap2", m->name);
        }
        return;
    }
    // 双补丁岛（近布局）：文件末尾 PATCH2_LEN 字节是前岛，放在映射起点（base − lb）前 FRONT 处，前岛末到映射起点的空隙 PROT_NONE
    uint64_t lb = m->load_bias, l2 = m->patch2_len, up2 = (l2 + 0x3FFF) & ~0x3FFFull;
    if (sz < l2) die("%s 比 PATCH2_LEN %#llx 短", path, (unsigned long long)l2);
    uint64_t pre = m->patch2_off < 0 ? (uint64_t)-m->patch2_off - lb : 0;
    if (up2 > pre) die("%s 前岛 %#llx 字节放不进映射起点前的 %#llx", path, (unsigned long long)l2, (unsigned long long)pre);
    m->size = (sz - l2 + 0x3FFF) & ~0x3FFFull;
    uint64_t at = mod_slot(m, pre + lb + m->size);   // 装载偏置：映射从 base − lb 起，base + DATA_OFF 落在宿主页边界
    int boot_slot = boot_map_slot(m);
    uint8_t *a0 = mmap((void *)at, pre + lb + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (at ? MAP_FIXED : 0), -1, 0);
    if (a0 == MAP_FAILED) die("%s mmap", m->name);
    boot_map_record(boot_slot, (uint64_t)a0, pre + lb + m->size);
    uint8_t *a = a0 + pre;
    m->base = a + lb;
    memcpy(m->base, raw, sz - l2); if (l2) memcpy(a0, raw + sz - l2, l2); free(raw); g_boot_raw = NULL; install_key_page(m);
    seal_rx(m, a, ((m->text_end + lb + 0x3FFF) & ~0x3FFFull), "text");   // [base − lb, 宿主页网格上的 text 末)：text 与（共页的）ro，只读可执行
    uint64_t po = (((uint64_t)m->patch_off + lb) & ~0x3FFFull) - lb;
    seal_rx(m, m->base + po, m->size - po, "patch");
    if (pre) {
        if (up2) seal_rx(m, a0, up2, "patch2");
        if (pre > up2 && mprotect(a0 + up2, pre - up2, PROT_NONE)) die("%s mprotect gap2", m->name);
    }
}
#endif

// ---------- rtld 交给 nn::ro 的全局（10-29 普查 c1029c：大航海4 HD / 魔界战记6 / 忍者龟装 NRO、AM2R 调 nn::ro::LookupSymbol 都读空指针）----------
// 真机 rtld 起局时给每个自动装载模块在 MOD0 指定处（MOD0 + 0x18 的偏移）建模块对象（与 SDK 的 nn::ro::detail::RoModule 同一结构），
//   串成环形链表，再按名字把五个交接全局写进每个定义它们的模块（原件 rtld 反汇编，魔界战记6 / AM2R 两份一致）：
//   g_pAutoLoadList = 自动装载表头、g_pManualLoadList = 手动装载表头（nn::ro 装的 NRO 挂这里）、g_pRoDebugFlag = 调试标志、
//   g_pLookupGlobalAutoFunctionPointer = 「按名字在自动装载模块里查全局符号」函数、g_pLookupGlobalManualFunctionPointer = rtld 的手动查符号槽（见 finish_modules）。
// 模块对象由游戏自己 SDK 的 RoModule::Initialize 填（与 rtld 同参：大小 0、布尔 false），字段布局随 SDK 版本变也对得上。
// 链表节点开头两个指针：+0 后向、+8 前向（sdk LoadModule 插到表尾、LookupSymbol 从表头 +8 起走到回到表头）
typedef struct ro_node { struct ro_node *prev, *next; } ro_node;
static ro_node g_ro_auto = {&g_ro_auto, &g_ro_auto}, g_ro_manual = {&g_ro_manual, &g_ro_manual};
static uint8_t g_ro_debug;
static int g_nauto;   // 自动装载模块数（起局装的 exefs 模块；之后 nn::ro 装的 NRO 接在 g_mod 后面，不在自动装载表里）
// rtld 的 LookupGlobalAuto：nn::ro 解析 NRO 导入时先调它（RoModule::ResolveSym），查不到再查手动装载表。与起局重定位同一查法（relocate 未定义导入那支）
static uint64_t ro_lookup_auto(const char *name) {
    uint64_t v = s2a_direct_lookup(name);
    for (int i = 0; !v && i < g_nauto; i++) { void *p = module_sym(&g_mod[i], name); if (p) v = (uint64_t)p; }
    if (!v) { void *f = s2a_libc_lookup(name); if (f) v = (uint64_t)sw_xstack_wrap(name, f); }
    return v;
}
// SDK 不导出初始化时调的那版（ro_find.h）多一个报错回调：动态段里有它不认的项（如 DT_SYMENT 不是 0x18）时调 (编号, 值)；SDK 自己的回调直接中止，宿主报红后照常往下
static void ro_init_bad(uint64_t code, uint64_t val) { LOG("【红】nn::ro 交接：游戏 SDK 的 RoModule 初始化报动态段异常 %#llx（值 %#llx）", (unsigned long long)code, (unsigned long long)val); }
// SDK 没导出 RoModule::Initialize 时，从导出的 nn::ro::LoadModule 找它内部调的那个（10-31 Minecraft：不建表则 nn::ro::LookupSymbol("vkCreateInstance") 读空表头崩在首帧前）
static void *ro_local_init(void) {
    static const char *const lm[2] = {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib"};
    for (int j = 0; j < 2; j++) for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i]; const uint32_t *p = module_sym(m, lm[j]); if (!p) continue;
        const char *why = NULL; const uint32_t *f = ro_find_init(p, (const uint32_t *)m->base, (const uint32_t *)(m->base + m->text_end), &why);
        if (f) { LOG("nn::ro 交接：游戏 SDK 不导出 RoModule::Initialize，用 LoadModule 里调的那个（%s+%#llx）", m->name, (unsigned long long)((const uint8_t *)f - m->base)); return (void *)f; }
        // 不打红：初始化内联进 LoadModule 的 SDK（10-31 扫现行根：smg2 / botw / acnh / silksong）没有单独函数可调，这几款不调 LookupSymbol、照常可玩
        LOG("nn::ro 交接：游戏 SDK 不导出 RoModule::Initialize，%s 的 LoadModule 里也没认出单独的初始化（%s），自动装载表留空（LookupSymbol 查不到符号）", m->name, why);
        return NULL;
    }
    if (guest_sym("_ZN2nn2ro6detail15g_pAutoLoadListE")) LOG("nn::ro 交接：游戏 SDK 有自动装载表头却既不导出 RoModule::Initialize 也不导出 LoadModule，自动装载表留空（LookupSymbol 查不到符号）");
    return NULL;
}
// RoModule 里「懒绑定入口」字段的偏移（rtld 填、SDK 的 Initialize 清零）：从游戏 SDK 导出的 RoModule::RestoreFunctions 读——
//   它把指向被卸模块的函数槽写回这个字段：ldr Xt, [x0, #偏移] 紧跟 str Xt, [Xn, Xm]。本机 11 份 SDK（10.x–20.x）都是 0xc8；读不出回 -1。
//   老 SDK（11-13 一款第一方游戏）函数名是单数 RoModule::RestoreFunction、字段在 +0xb0，同样两条指令；
//   更新的 SDK（11-13 两款 Switch 2 游戏）多一个回调参数 RestoreFunctions(RoModule*, void (*)(const char*))、开头先有一条 retab 早退，字段仍在 +0xc8：三个名字都认
//   扫描范围按导出符号的 st_size 圈定 [入口, 入口 + st_size)：新 SDK 还有一种块布局把「表为空直接返回」的早退 ret（不是 retab）放在开头、那两条在后面
//   （11-14 本机 6 款 sdk 里 4 款如此，原先遇第一条 ret 就停、回 -1，懒绑定入口没交接：卸 NRO 后主程序调它的函数跳 0）。
//   st_size 为 0（符号没带大小）才退回遇 ret 即停；范围越出映像、或认出两个不同偏移就报红回 -1，不猜
static long ro_stub_off(void) {
    static const char *const names[3] = {"_ZN2nn2ro6detail8RoModule16RestoreFunctionsEPS2_", "_ZN2nn2ro6detail8RoModule16RestoreFunctionsEPS2_PFvPKcE",
                                         "_ZN2nn2ro6detail8RoModule15RestoreFunctionEPS2_"};
    const module_t *m = NULL; const Elf64_Sym *e = NULL;
    for (int k = 0; k < 3 && !e; k++) for (int i = 0; i < g_nmod && !e; i++) if ((e = module_sym_ent(&g_mod[i], names[k]))) m = &g_mod[i];
    if (!e) return -1;
    uint64_t n = e->st_size / 4, lim = n ? n : 0x100;
    uint64_t end = m->patch_off < 0 ? m->image_size : m->key_page_off;   // 映像末（同 ro_handoff 的模块对象边界）
    if (e->st_value + lim * 4 > end) { LOG("【红】nn::ro 交接：%s 的 RestoreFunctions（+%#llx，%llu 字节）越出模块映像，不读", m->name, (unsigned long long)e->st_value, (unsigned long long)e->st_size); return -1; }
    const uint32_t *f = (const uint32_t *)(m->base + e->st_value);
    long found = -1;
    for (uint64_t i = 1; i < lim; i++) {
        if (!n && f[i - 1] == 0xd65f03c0) break;
        uint32_t s = f[i], l = f[i - 1];
        if ((s & 0xffe00c00) == 0xf8200800 && (l & 0xffc00000) == 0xf9400000 && ((l >> 5) & 31) == 0 && (l & 31) == (s & 31)) {
            long off = (long)((l >> 10) & 0xfff) * 8;
            if (found >= 0 && off != found) { LOG("【红】nn::ro 交接：%s 的 RestoreFunctions 里认出两个懒绑定入口偏移（+%#lx / +%#lx），不猜", m->name, (unsigned long)found, (unsigned long)off); return -1; }
            found = off;
        }
    }
    return found;
}
static void ro_handoff(void) {
    g_nauto = g_nmod;
    void *init5 = guest_sym("_ZN2nn2ro6detail8RoModule10InitializeEmmPKNS1_3Elf5Elf643DynEb");   // (基址, 大小, 动态段, 布尔)
    void *init2 = init5 ? NULL : guest_sym("_ZN2nn2ro6detail8RoModule10InitializeEmPKNS1_3Elf5Elf643DynE");   // 旧 SDK：(基址, 动态段)
    void *init6 = init5 || init2 ? NULL : ro_local_init();   // 不导出的：(基址, 大小, 动态段, 布尔, 报错回调)
    if (!init5 && !init2 && !init6 && !guest_sym("_ZN2nn2ro6detail15g_pAutoLoadListE")) { LOG("nn::ro 交接：游戏 SDK 没有 RoModule::Initialize（不用 nn::ro），不建自动装载表"); return; }
    // 有交接全局却建不了模块对象（初始化内联进 LoadModule 的 SDK）：全局照写、自动装载表留空环——LookupSymbol 返回「找不到」而不是读空表头，nn::ro 装 NRO 照常挂手动装载表
    void *bad = init6 ? sw_xstack_wrap("nn::ro RoModule 初始化报错", (void *)ro_init_bad) : NULL;
    unsigned n = 0, nlazy = 0; long stub_off = ro_stub_off(); void *bind = guest_sym("_ZN2nn2ro9BindEntryEv");
    for (int i = 0; (init5 || init2 || init6) && i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        uint32_t mod0 = *(uint32_t *)(m->base + 4); int32_t rel = *(int32_t *)(m->base + mod0 + 0x18);
        uint64_t off = (uint64_t)((int64_t)mod0 + rel), end = m->patch_off < 0 ? m->image_size : m->key_page_off;
        if (!rel || off < m->data_off || off + 0xd0 > end) { LOG("【红】nn::ro 交接：%s 的模块对象偏移 %#llx 不在数据段 [%#llx, %#llx)，不进自动装载表", m->name, (unsigned long long)off, (unsigned long long)m->data_off, (unsigned long long)end); continue; }
        ro_node *o = (ro_node *)(m->base + off);
        if (init5) ((void (*)(void *, uint64_t, uint64_t, const void *, int))init5)(o, (uint64_t)m->base, 0, m->dyn, 0);
        else if (init2) ((void (*)(void *, uint64_t, const void *))init2)(o, (uint64_t)m->base, m->dyn);
        else {
            ((void (*)(void *, uint64_t, uint64_t, const void *, int, void *))init6)(o, (uint64_t)m->base, 0, m->dyn, 0, bad);
            int hb = 0, hd = 0;   // 找来的函数参数顺序是按一版 SDK 认的：调完核对象里确实记下了基址与动态段，对不上不进表（宁可不建也不挂错对象）
            for (const uint64_t *q = (const uint64_t *)o + 2; q < (const uint64_t *)((uint8_t *)o + 0xd0); q++) { hb |= *q == (uint64_t)m->base; hd |= *q == (uint64_t)m->dyn; }
            if (!hb || !hd) { LOG("【红】nn::ro 交接：%s 的模块对象里没有基址 / 动态段（%d / %d），找来的初始化参数不对，不进自动装载表", m->name, hb, hd); continue; }
        }
        o->prev = g_ro_auto.prev; o->next = &g_ro_auto; g_ro_auto.prev->next = o; g_ro_auto.prev = o; n++;
        // rtld 的懒绑定交接（同 SDK RoModule::Relocation 末尾）：模块对象记 PLT 表头，GOT[1] = 模块对象、GOT[2] = nn::ro::BindEntry。
        //   缺了它，游戏的 nn::ro 卸 NRO 时把本模块指向它的函数槽改回 0，重装后一调就跳 0（#858：换角色后 pc 0、lr 在 main）
        if (m->plt_stub && stub_off >= 0) *(uint64_t *)((uint8_t *)o + stub_off) = m->plt_stub;
        if (m->pltgot && bind) { uint64_t *got = (uint64_t *)(m->base + m->pltgot); got[1] = (uint64_t)o; got[2] = (uint64_t)bind; nlazy++; }
    }
    if (n && (stub_off < 0 || !bind)) LOG("【红】nn::ro 交接：游戏 SDK 里认不出懒绑定交接（RestoreFunctions 偏移 %ld、BindEntry %s）：卸 NRO 后主程序调它的函数会跳 0", stub_off, bind ? "有" : "没有");
    void *look = sw_xstack_wrap("nn::ro LookupGlobalAuto", (void *)ro_lookup_auto);
    static const char *const k[4] = {"_ZN2nn2ro6detail15g_pAutoLoadListE", "_ZN2nn2ro6detail17g_pManualLoadListE",
                                     "_ZN2nn2ro6detail14g_pRoDebugFlagE", "_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE"};
    const uint64_t v[4] = {(uint64_t)&g_ro_auto, (uint64_t)&g_ro_manual, (uint64_t)&g_ro_debug, (uint64_t)look};
    unsigned w = 0;
    for (int i = 0; i < g_nmod; i++) for (int j = 0; j < 4; j++) { uint64_t *p = module_sym(&g_mod[i], k[j]); if (p) { *p = v[j]; w++; } }
    LOG("nn::ro 交接：自动装载表 %u 个模块（RoModule::Initialize %s），交接全局写了 %u 处；懒绑定交接 %u 个模块（入口字段 +%#lx）", n, init5 ? "导出四参" : init2 ? "导出二参" : init6 ? "LoadModule 里的五参" : "没有，表留空", w, nlazy, (unsigned long)stub_off);
}
static uint64_t g_rtld_lookup_manual_slot;
static void finish_modules(void) {
    for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]);
    ro_handoff();
    // rtld 职责：各模块的 nn::ro::detail::g_pLookupGlobalManualFunctionPointer（初值 0、无重定位）指到 rtld 的「手动装载模块查符号」槽。
    // 不装 rtld、也没有手动装载的模块：给一个宿主自有的槽，nn::ro 写进来的指针没人读（真机 rtld 只在解析手动模块时调它）
    for (int i = 0; i < g_nmod; i++) { uint64_t *p = module_sym(&g_mod[i], "_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE"); if (p && !*p) *p = (uint64_t)&g_rtld_lookup_manual_slot; }
    if (g_unresolved) LOG("【红】装载：%u 个导入没有任何模块定义（见上）", g_unresolved);
}
static char g_moddir[1024];
const char *s2a_module_dir(void) { return g_moddir; }
void s2a_module_dir_set(const char *dir) { strlcpy(g_moddir, dir, sizeof g_moddir); }   // 起局早期就定下目录：图形模式判定（gfxmode.c decide）在装模块之前读 main.npdm 取程序号
// 直连模式不装的 GPU 驱动模块（NVIDIA OpenGL / Vulkan 驱动：经 NvRm 编程 GPU，宿主没有；游戏对它们的调用由直连图形层答，装了反而先被它们接走）。
//   名单 gpu_drivers.tsv 由出包按模块自身的 NintendoSDK 组件判（graphics/nvn/games/profile_guess.py gpu_driver_modules，与推导器同一判法），不读手写档；
//   通用模式（Ryujinx GPU 模拟）照装：游戏靠它们往 nvdrv 发 GPU 命令。其余模块与 Horizon 一样全装
static int gpu_driver_skip(const char *dir, const char *name) {
    if (s2a_gfx_mode() != S2A_GFX_DIRECT) return 0;
    // 出包每款必写这张表（pack/stage_rewrite.sh、runtime/run_mac.sh，没有驱动模块就是空表）：不在 / 打不开 / 读不了都是包坏，
    //   不能当「没有驱动模块」照装——装上的驱动会先接走游戏的图形调用（审计 P2-7）
    char p[1100], l[64]; snprintf(p, sizeof p, "%s/gpu_drivers.tsv", dir); FILE *f = fopen(p, "r");
    if (!f) die("【红】直连：GPU 驱动模块名单 %s 打不开：%s（出包每款必写，缺了 / 打不开不能当没有驱动模块照装）", p, strerror(errno));
    int hit = 0; while (!hit && fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; hit = l[0] != '#' && !strcmp(l, name); }
    int rerr = ferror(f) ? errno : 0; fclose(f);
    if (rerr) die("【红】直连：GPU 驱动模块名单 %s 读失败：%s", p, strerror(rerr));
    return hit;
}
#if !defined(__ANDROID__)
static int nro_table_count(void);   // 读 nro_modules.tsv（g_moddir，只读一次），回行数（nn::ro 一节）
#endif
void load_modules(const char *dir, const char **names_in, int n_in) {
    if (!s2a_module_order_ok(names_in, n_in)) die("【红】装载：模块名单不是 Horizon 序（main → subsdk0..N → sdk）");
    strlcpy(g_moddir, dir, sizeof g_moddir);
    const char **names = calloc(n_in > 0 ? (size_t)n_in : 1, sizeof *names); int n = 0;
    if (!names) die("模块名表分配失败");
    for (int i = 0; i < n_in; i++) {
        if (gpu_driver_skip(dir, names_in[i])) { LOG("直连：%s 是 GPU 驱动模块（gpu_drivers.tsv），不装——游戏对它的调用由直连图形层答", names_in[i]); continue; }
        names[n++] = names_in[i];
    }
    int cap = n;
#if !defined(__ANDROID__)
    cap += nro_table_count();   // nn::ro 能装的 NRO 全在 nro_modules.tsv：模块表一次留够
#endif
    mod_table_alloc(cap);
    for (int i = 0; i < n; i++) { strlcpy(g_mod[i].name, names[i], sizeof g_mod[i].name); read_layout(&g_mod[i], dir); }
    create_keys();
    if (plat_tsd_init()) die("TSD 初始化失败");
#if !defined(__ANDROID__)
    if (s2a_window_ok()) {   // 模块段起点由布局定（layout.c：标准布局在各区之后，窗口放不下标准布局时在窗口最前）
        uint64_t s2a_mem_mod_base(const char *npdm); char np[1100]; snprintf(np, sizeof np, "%s/main.npdm", dir);
        uint64_t wb, ws, span = s2a_module_span(dir);
        if (!span) die("模块跨度为 0（%s 里没有可用的 *.fixups.tsv）", dir);
        g_modcur = s2a_mem_mod_base(np); s2a_window_get(&wb, &ws);
        LOG("模块段：窗口 %#llx 起第 %llu MB 处，%llu MB（各模块按 2 MB 对齐排）", (unsigned long long)wb, (unsigned long long)((g_modcur - wb) >> 20), (unsigned long long)(span >> 20));
    }
#endif
    for (int i = 0; i < n; i++) {   // 先全部映射并解析符号表，再统一重定位（跨模块符号要先都在位）
        module_t *m = &g_mod[i];
        map_image(m, dir);
        if (!i) tsd_check();
        parse_dynamic(m); register_gates(m, dir);
        { void s2a_patch_data_apply(module_t *); s2a_patch_data_apply(m); }   // 补丁 [data] 段（正确性类，无条件生效）：先于满屏装载，满屏数据点与之重叠的让给它
        { void s2a_widen_load(module_t *, const char *); s2a_widen_load(m, dir); }   // 宽屏点：填值槽、改 f32 数据（有 <名>.widen.tsv 才做）
        { void s2a_cpatch_load(module_t *, const char *); s2a_cpatch_load(m, dir); }   // 社区补丁：按设置写本模块掩码（槽 2046）与开着的数据补丁（有 cpatch.tsv 才做）
        LOG("%s: 基址 %p 大小 %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    s2a_nro_table_load(dir);   // 模块目录的 nro_exports.tsv（出包 tools/nro_exports.py 写；没有 romfs/nro 的款没有这张表）
    finish_modules();          // 统一重定位、填 rtld 手动装载查符号槽、报未解析导入
    if (g_deferred) LOG("装载：%u 个导入延迟到 nn::ro 装载（romfs NRO 导出，游戏用 nn::ro 装进来时才解析；起局前没装就被调到照旧报红中止）", g_deferred);
    free(names);
}
#if defined(__ANDROID__)
static struct s2a_loader_blob read_asset(const struct s2a_loader_source *source, const char *name, const char *suffix) {
    char filename[128];
    if (snprintf(filename, sizeof filename, "%s.%s", name, suffix) >= (int)sizeof filename)
        die("APK 模块文件名过长");
    struct s2a_loader_blob blob = {0};
    int error = source->read(source->context, filename, &blob);
    if (error || (!blob.bytes && blob.length)) die("APK 读取 %s 失败 rc=%d", filename, error);
    return blob;
}
void load_modules_from_source(const struct s2a_loader_source *source, const char **names, int n) {
    if (!source || !source->read || !source->release || !names || n <= 0 || g_nmod)
        die("APK 装载输入无效或进程已经装载过模块");
    if (!s2a_module_order_ok(names, n)) die("【红】APK 装载：模块名单不是 Horizon 序（main → subsdk0..N → sdk）");
    struct s2a_loader_blob *fixups = calloc((size_t)n, sizeof *fixups);
    if (!fixups) die("APK 随行表数组分配失败");
    mod_table_alloc(n);   // 安卓不装 NRO（s2a_nro_map 回 5）：模块表 = 起局模块数
    for (int i = 0; i < n; i++) {
        if (!names[i] || !*names[i] || strlen(names[i]) >= sizeof g_mod[i].name ||
            strspn(names[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(names[i]))
            die("APK 模块名无效");
        for (int j = 0; j < i; j++) if (!strcmp(names[i], names[j])) die("APK 模块名重复：%s", names[i]);
        strlcpy(g_mod[i].name, names[i], sizeof g_mod[i].name);
        fixups[i] = read_asset(source, names[i], "fixups.tsv");
        struct loader_text text = {.bytes = fixups[i].bytes, .length = fixups[i].length, .what = names[i]};
        read_layout_text(&g_mod[i], &text);
    }
    // Policy bytes and module sidecars are read through the same authenticated source.
    void s2a_widen_source_begin(void);
    void s2a_widen_source_policy(unsigned, const void *, size_t);
    void s2a_widen_load_source(module_t *, const void *, size_t);
    static const char *const policies[] = {"widen_enable.handheld.tsv", "widen_enable.docked.tsv", "widen_full.handheld.tsv", "widen_full.docked.tsv", "widen_verified.tsv"};
    s2a_widen_source_begin();
    for (unsigned i = 0; i < sizeof policies / sizeof policies[0]; i++) {
        struct s2a_loader_blob blob = {0};
        int rc = source->read(source->context, policies[i], &blob);
        if (rc == ENOENT) continue;
        if (rc || (!blob.bytes && blob.length)) die("APK 读取 %s 失败 rc=%d", policies[i], rc);
        s2a_widen_source_policy(i, blob.bytes, blob.length);
        source->release(source->context, &blob);
    }
    create_keys();
    if (plat_tsd_init()) die("TSD 初始化失败");
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        struct s2a_loader_blob image = read_asset(source, names[i], "patched.bin");
        uint64_t offsets[KEY_N]; mapping_offsets(m, offsets);
        struct s2a_module mapped; char err[512];
        int rc = s2a_module_map_mem(names[i], fixups[i].bytes, fixups[i].length, image.bytes, image.length,
                                    offsets, &mapped, err, sizeof err);
        source->release(source->context, &image);
        source->release(source->context, &fixups[i]);
        if (rc) die("%s: APK 映射失败 rc=%d %s", names[i], rc, err);
        adopt_mapping(m, &mapped);
        if (!i) tsd_check();
        parse_dynamic(m);
        struct s2a_loader_blob gates = read_asset(source, names[i], "gates.tsv");
        struct loader_text text = {.bytes = gates.bytes, .length = gates.length, .what = names[i]};
        register_gates_text(m, &text);
        source->release(source->context, &gates);
        char widen_name[128];
        if (snprintf(widen_name, sizeof widen_name, "%s.widen.tsv", names[i]) >= (int)sizeof widen_name)
            die("APK 宽屏表文件名过长");
        struct s2a_loader_blob widen = {0};
        int widen_rc = source->read(source->context, widen_name, &widen);
        if (widen_rc != ENOENT) {
            if (widen_rc || (!widen.bytes && widen.length)) die("APK 读取 %s 失败 rc=%d", widen_name, widen_rc);
            s2a_widen_load_source(m, widen.bytes, widen.length);
            source->release(source->context, &widen);
        }
        LOG("%s: APK 基址 %p 大小 %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    free(fixups);
    finish_modules();
}
#endif
unsigned s2a_unresolved_count(void) { return g_unresolved; }

// ---------- nn::ro 装 NRO（ldr:ro LoadNro / UnloadNro；services 的 IRoInterface 经 S2AHooks 调这里）----------
// 真机：ro 把游戏读进内存的 NRO 映射成代码，游戏的 nn::ro 再自己重定位、挂手动装载表、调 DT_INIT。
// iOS 不许运行时装入未签名的代码执行：NRO 在出包时与 exefs 模块一样改写、写进签名的外置镜像包（pack/stage_rewrite.sh、runtime/rewriter/nro_to_nso.py、ios/embed.py），
//   这里按 NRO 文件的 SHA-256 在 nro_modules.tsv 里认出模块，映射改写好的映像（与起局模块同一 map_image，落在窗口模块段：s2a_module_span 已把它算进去），
//   登记门，返回基址；重定位、DT_INIT 照真机由游戏的 nn::ro 做。卸了再装同一个 NRO：映回原地址、数据段重新取干净副本
typedef struct { char name[16]; uint8_t sha[32]; uint64_t size, bss; int mod, loaded; } nro_mod_t;
static nro_mod_t *g_nro; static int g_nnro = -1, g_cnro;   // nro_modules.tsv 的行，按需增长（无上限）
static pthread_mutex_t g_nro_mu = PTHREAD_MUTEX_INITIALIZER;
#if !defined(__ANDROID__)   // NRO 映射表只在非安卓路径读（安卓 s2a_nro_map 直接回 5：模块映射走 APK 资源）
static int hexbyte(const char *p) { int v = 0; for (int i = 0; i < 2; i++) { char c = p[i]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1000); } return v; }
static void nro_table(void) {   // 锁内调：起局（load_modules 定模块表容量）时读模块目录的 nro_modules.tsv（没有 = 本款没有 NRO），之后不再读
    if (g_nnro >= 0) return;
    g_nnro = 0;
    char p[1100], l[2048]; snprintf(p, sizeof p, "%s/nro_modules.tsv", g_moddir); FILE *f = fopen(p, "r"); if (!f) return;
    while (fgets(l, sizeof l, f)) {
        char name[16], sha[65]; unsigned long long size, bss; int bad = 0;
        if (l[0] == '#' || sscanf(l, "%15s %64s %llx %llx", name, sha, &size, &bss) != 4 || strlen(sha) != 64) continue;
        if (g_nnro == g_cnro) {
            int nc = g_cnro ? g_cnro * 2 : 64; nro_mod_t *q = realloc(g_nro, (size_t)nc * sizeof *q);
            if (!q) die("nn::ro：nro_modules.tsv 第 %d 行起的表分配失败", g_nnro + 1);
            g_nro = q; g_cnro = nc;
        }
        nro_mod_t *e = &g_nro[g_nnro];
        for (int i = 0; i < 32; i++) { int b = hexbyte(sha + 2 * i); if (b < 0) bad = 1; e->sha[i] = (uint8_t)b; }
        if (bad) continue;
        strlcpy(e->name, name, sizeof e->name); e->size = size; e->bss = bss; e->mod = -1; e->loaded = 0; g_nnro++;
    }
    fclose(f);
    LOG("nn::ro：nro_modules.tsv %d 个 NRO 模块", g_nnro);
}
static int nro_table_count(void) { pthread_mutex_lock(&g_nro_mu); nro_table(); int n = g_nnro; pthread_mutex_unlock(&g_nro_mu); return n; }
#endif
// 0 = 成功（*base = 映射基址）；1 = 包里没有这个 NRO（出包时 romfs 里没有 / 没改写进包）；2 = 大小或 bss 与包里不符；3 = 已经装着；4 = 模块表满；5 = 本平台未支持
__attribute__((visibility("default"), used)) int s2a_nro_map(const uint8_t sha[32], uint64_t size, uint64_t bss, uint64_t *base) {
#if defined(__ANDROID__)
    (void)sha; (void)size; (void)bss; (void)base;
    LOG("【红】nn::ro 装 NRO：Android 未支持（模块映射走 APK 资源，见 load_modules_from_source）"); return 5;
#else
    pthread_mutex_lock(&g_nro_mu); nro_table();
    nro_mod_t *e = NULL; for (int i = 0; i < g_nnro; i++) if (!memcmp(g_nro[i].sha, sha, 32)) e = &g_nro[i];
    int rc = 0;
    if (!e) { rc = 1; LOG("【红】nn::ro 装 NRO：哈希 %02x%02x%02x%02x… 不在 nro_modules.tsv（包里没有这个 NRO 的改写映像：出包时 romfs 里没有它？）", sha[0], sha[1], sha[2], sha[3]); }
    else if (e->size != size || e->bss != bss) { rc = 2; LOG("【红】nn::ro 装 NRO：%s 大小 %#llx / bss %#llx 与包里 %#llx / %#llx 不符", e->name, (unsigned long long)size, (unsigned long long)bss, (unsigned long long)e->size, (unsigned long long)e->bss); }
    else if (e->loaded) rc = 3;
    else if (e->mod < 0 && g_nmod >= g_mod_cap) { rc = 4; LOG("【红】nn::ro 装 NRO：模块表满（%d）装不下 %s（模块表按起局模块 + nro_modules.tsv 行数分配，不该发生）", g_mod_cap, e->name); }
    else if (e->mod < 0) {   // 第一次装：读布局、映射、建符号表、登记门，最后才把模块数加一（别的线程按 g_nmod 遍历模块表）
        module_t *m = &g_mod[g_nmod]; memset(m, 0, sizeof *m); strlcpy(m->name, e->name, sizeof m->name);
        read_layout(m, g_moddir); map_image(m, g_moddir); parse_dynamic(m); register_gates(m, g_moddir);
        e->mod = g_nmod; __atomic_store_n(&g_nmod, g_nmod + 1, __ATOMIC_RELEASE);
        e->loaded = 1; *base = (uint64_t)m->base;
        size_t nb = 0, left = 0, nfail = 0;   // 起局没解析、这个 NRO 定义了的导入：改指过来（符号地址 = 基址 + 值，与 NRO 自己的重定位无关，映射完就定）
        //   槽可能已被游戏设成只读（DT_INIT 后 SetMemoryPermission 把 RELRO 收紧）：经 svc_mem.c s2a_loader_store64 写（只读数据页临时加写再恢复），写不进就留着报错桩、打红，不崩整个进程
        int s2a_loader_store64(uint64_t *where, uint64_t v);
        for (size_t i = 0; i < g_npend; i++) if (g_pend[i].where) {
            void *p = module_sym(m, g_pend[i].name); if (!p) { left++; continue; }
            if (s2a_loader_store64(g_pend[i].where, (uint64_t)p + (g_pend[i].slot && !jump_slot_addend() ? 0 : (uint64_t)g_pend[i].addend))) {
                if (!nfail++) LOG("【红】nn::ro 装 NRO %s：导入 %s 的槽 %p 写不进（页不可写且不是可放开的数据页），留着报错桩", e->name, g_pend[i].name, (void *)g_pend[i].where);
                left++; continue; }
            g_pend[i].where = NULL; nb++;
        }
        LOG("nn::ro 装 NRO %s：基址 %p 大小 %#zx；起局没解析的导入补绑 %zu 个（还剩 %zu 个待绑，其中写不进 %zu 个）", e->name, m->base, m->size, nb, left, nfail);
    } else {   // 卸了再装：映回原地址（数据段、bss 重新是包里的干净副本；代码页、符号表、门不变；iOS 只换可写部分，见 remap_writable）
        module_t *m = &g_mod[e->mod];
#if TARGET_OS_IPHONE
        remap_writable(m);
#else
        g_slot_at = (uint64_t)m->base - m->load_bias - (m->patch_off < 0 ? (uint64_t)-m->patch_off : 0);
        uint8_t *was = m->base; map_image(m, g_moddir);
        if (m->base != was) die("nn::ro 重装 %s：映射到 %p，不是原地址 %p", e->name, m->base, was);
#endif
        e->loaded = 1; *base = (uint64_t)m->base;
        LOG("nn::ro 重装 NRO %s：基址 %p", e->name, m->base);
    }
    pthread_mutex_unlock(&g_nro_mu); return rc;
#endif
}
// UnloadNro：标记卸下（映像留在原地址，下次装同一个 NRO 映回同址）。0 = 成功，1 = 这个基址没有装着的 NRO
__attribute__((visibility("default"), used)) int s2a_nro_unmap(uint64_t base) {
    pthread_mutex_lock(&g_nro_mu); int rc = 1;
    for (int i = 0; i < g_nnro; i++) if (g_nro[i].loaded && g_nro[i].mod >= 0 && (uint64_t)g_mod[g_nro[i].mod].base == base) { g_nro[i].loaded = 0; rc = 0; LOG("nn::ro 卸 NRO %s：基址 %#llx", g_nro[i].name, (unsigned long long)base); }
    pthread_mutex_unlock(&g_nro_mu); return rc;
}
// The coordinator replaces guest pages before releasing loader registrations.
// Signed iOS images are remapped from their pristine original segments.
void s2a_loader_session_reset(void) {
    for (int i = 0; i < g_mod_cap; i++) free(g_mod[i].symh);
    free(g_mod); g_mod = NULL; g_mod_cap = 0; g_nmod = 0;
#if defined(__APPLE__)
    g_modcur = g_slot_at = 0;
#endif
    free(g_pend); g_pend = NULL; g_npend = g_cpend = 0;
    g_unresolved = g_deferred = 0;
    g_main_build_id[0] = 0; g_rtld_lookup_manual_slot = 0;
    void s2a_nro_table_reset(void); s2a_nro_table_reset();
    free(g_nro); g_nro = NULL; g_cnro = 0; g_nnro = -1; g_nauto = 0; g_ro_debug = 0;   // nn::ro：下次重读表，旧映像的自动/手动对象不再可达
    g_ro_auto.prev = g_ro_auto.next = &g_ro_auto; g_ro_manual.prev = g_ro_manual.next = &g_ro_manual;
}

#if defined(__APPLE__)
// The boot owner has stopped its readers, released registrations and restored
// the guest reservation. Release any mappings outside it, then host metadata.
int s2a_loader_abort_boot(void) {
    if (!g_boot_owned) return -1;
    uint64_t wb, ws; s2a_window_get(&wb, &ws);
    int reserved = s2a_window_ok();
    for (int i = 0; g_boot_maps && i < g_mod_cap; i++) {
        module_t *m = &g_mod[i];
        uint64_t at = g_boot_maps[i].at, length = g_boot_maps[i].length;
        if (!length) continue;
        if (reserved && at >= wb && at - wb < ws && length <= ws - (at - wb)) {
            m->base = NULL;
            g_boot_maps[i].at = g_boot_maps[i].length = 0;
            continue;
        }
        if (reserved && at < wb + ws && (at >= wb || length > wb - at)) {
            LOG("【红】失败装载映射跨越客体窗口边界，保留所有权：%s %#llx+%#llx", m->name,
                (unsigned long long)at, (unsigned long long)length);
            return -1;
        }
        if (munmap((void *)at, length)) {
            LOG("【红】失败装载映射归还失败：%s errno=%d，保留所有权", m->name, errno);
            return -1;
        }
        m->base = NULL;
        g_boot_maps[i].at = g_boot_maps[i].length = 0;
    }
    s2a_loader_session_reset();
    free(g_boot_maps); g_boot_maps = NULL;
    g_boot_owned = 0;
    return 0;
}
#endif

// rtld 的 InitModules：按装载链表逆序调各模块 DT_INIT（sdk → subsdk* → main，与依赖方向一致）
void run_module_inits(void) {
    for (int i = g_nmod - 1; i >= 0; i--) {
        module_t *m = &g_mod[i]; if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx", m->name, (unsigned long long)m->init);
        ((void (*)(void))(m->base + m->init))();
    }
}
