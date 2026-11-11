// S2Arm 直连库打包容器的共用核（#1434 拆 lib_pack.inc：容器格式、lib.idx 打开与核头、按键二分——两端同源）。
//   b-<链版本>/lib.idx：头 48 字节（偏移 0 "S2ALIB\0\0"、8 u32 ver = 1、12 u32 n、16 u32 nshard、20 u32 0、24 char chain[12]、36–47 为 0）+ nshard × 16 字节分片表（u64 size、u64 0）
//   + n × 24 字节记录 {u64 key; u8 kind; u8 shard; u16 0; u32 len; u64 off;}，按 (key, kind) 升序；b-<链版本>/lib.NNN.pak：各段原样字节。
//   kind：0 _b.metallib、1 _b.json、2 _bf.metallib、3 _bv.metallib、4+d _b_t<d>.metallib（曲面外壳的另外几种域，d = 0–2）、16+i _bg<i>.metallib、32+i _bg<i>.json、48 _bgv.metallib、49 _bgv.json、50 _ft.metallib（片元输出类型变体，入口 main0_t<u>_<s>）、
//   52 _b.spv（#783 安卓 Vulkan 产物，与种类 1 同键成对）、64+i _bgs<i>.metallib、80+i _bgs<i>.json、96 _bgc.metallib、97 _bgc.json（直通几何快路，Dsh.cs GeometryFinish）。
// 段落在哪个分片、偏移、长度由记录给出；分片怎么映射/读（iOS 文件映射进宿主高位区 + dispatch_data、安卓 POSIX mmap）是平台段，
//   平台段各自在 lib_pack.inc（Apple）/ libpack_vk.c（安卓）实现。包含方先定义 LP_LOG(fmt, ...)（一行日志，红行自带 【红】）。
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LP_K_B = 0, LP_K_BJSON = 1, LP_K_BF = 2, LP_K_BV = 3, LP_K_BT = 4, LP_K_BG = 16, LP_K_BGJSON = 32, LP_K_BGV = 48, LP_K_BGVJSON = 49, LP_K_FT = 50, LP_K_SE = 51,
       LP_K_BSPV = 52,
       LP_K_BGS = 64, LP_K_BGSJSON = 80, LP_K_BGC = 96, LP_K_BGCJSON = 97, LP_SHARD_MAX = 256 };   // 64+i / 80+i / 96 / 97: the passthrough-geometry fast path's <key>_bgs<i> (side-effect-only kernel) and <key>_bgc (capture reader)   // 51: a shader's sampler-emulation variant <key>_se, precompiled by the package (nvn_mtl.m se_packed)   // 52: per-key Vulkan SPIR-V beside _b.json (graphics/tools/direct/vklib)
typedef struct { uint64_t key; uint8_t kind, shard; uint16_t z; uint32_t len; uint64_t off; } lp_rec;
_Static_assert(sizeof(lp_rec) == 24, "lib.idx record is 24 bytes");
typedef struct {
    const uint8_t *idx; size_t idx_len; uint32_t n, nshard; const lp_rec *rec; const uint64_t *ssize;
    char dir[1024];
} lp_pack;

// 打开 <dir>/lib.idx 并核头：魔数、版本、链版本（chain 与 want_chain 同为 12 字符）、长度。成功返回 1；没有 lib.idx 返回 0（不报红，由调用方决定）；格式不对报红返回 -1
static int lp_open(lp_pack *p, const char *dir, const char *want_chain) {
    memset(p, 0, sizeof *p); snprintf(p->dir, sizeof p->dir, "%s", dir);
    char path[1100]; snprintf(path, sizeof path, "%s/lib.idx", dir);
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) || st.st_size < 48) { close(fd); LP_LOG("【红】 direct library container: %s is %lld bytes, shorter than its 48-byte header", path, (long long)st.st_size); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { LP_LOG("【红】 direct library container: cannot map %s", path); return -1; }
    const uint8_t *h = m; uint32_t ver, n, ns; memcpy(&ver, h + 8, 4); memcpy(&n, h + 12, 4); memcpy(&ns, h + 16, 4);
    char chain[13] = {0}; memcpy(chain, h + 24, 12);
    const char *why = memcmp(h, "S2ALIB\0\0", 8) ? "magic is not S2ALIB" : ver != 1 ? "format version is not 1" : ns > LP_SHARD_MAX ? "too many shards"
                    : (size_t)st.st_size != 48 + 16 * (size_t)ns + 24 * (size_t)n ? "length does not match the record count" : want_chain && strcmp(chain, want_chain) ? "translation chain differs" : NULL;
    if (why) { LP_LOG("【红】 direct library container %s: %s (version %u, %u records, %u shards, chain %s, this host expects %s)", path, why, ver, n, ns, chain, want_chain ? want_chain : "-");
               munmap(m, (size_t)st.st_size); return -1; }
    p->idx = h; p->idx_len = (size_t)st.st_size; p->n = n; p->nshard = ns;
    p->ssize = (const uint64_t *)(h + 48); p->rec = (const lp_rec *)(h + 48 + 16 * (size_t)ns);   // the header and the shard table are 16-byte multiples: records stay 8-byte aligned
    return 1;
}
static const lp_rec *lp_find(const lp_pack *p, uint64_t key, unsigned kind) {
    size_t lo = 0, hi = p->n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; const lp_rec *r = &p->rec[mid];
        if (r->key < key || (r->key == key && r->kind < kind)) lo = mid + 1; else if (r->key == key && r->kind == kind) return r; else hi = mid; }
    return NULL;
}
