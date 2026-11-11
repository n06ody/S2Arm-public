// S2Arm 直连库打包容器的 Apple 段读法（10-25 合库，格式见 lib_pack_core.inc 与共享笔记「画面着色器线_直连库合库草案_1025.md」§1，出库 graphics/tools/direct/lib_pack.py）。
//   容器头/记录结构、lib.idx 打开核头、按键二分在 lib_pack_core.inc（#1434 拆出共用，安卓侧 POSIX 段在 android/native/direct/libpack_vk.c 复用同一份）。
// 运行时：lib.idx 整份 mmap、二分查；一段 = 分片文件里这一段的只读文件映射（按页对齐只映射这一段，dispatch_data 放掉时 munmap），
//   给 newLibraryWithData（库）或 NSJSONSerialization（json；dispatch_data 与 NSData 免费桥接）。
//   分片不整片映射：10-26 巫师3 测试设备2 两局，整片映射且常驻，21 片 1343 MB 全落在低区（4 GB 到游戏窗口底，占满窗口的款只剩 1.1–1.4 GB），
//   低区吃光后宿主 C++ 分配抛异常 abort；而用到的只是各片里少数几段。现在只占活着的库那几段
//   一段原先是 malloc + pread 的堆内存：Metal 库对象持有它，库活多久它就常驻多久、全算脏页。11-01 无主之地3 第 60 秒宿主 malloc 1694 MB 里 1376 MB 是这些段
//   （659 段、均 2.1 MB，预热线程载入的 943 MB），常驻 5.29 GB 超 iPhone 5 GB 上限；Mac 上大块 malloc 的地址房间也被吃光，再取 2.8 / 4.9 MB 的段 malloc 返回空、库读不出。
//   改成文件映射：页是文件后备的干净页，系统可回收、不计入常驻（iOS 判杀看的足迹），地址占用与原先相同（只映射这一段）
// 段放高位区（11-05 赤痕 / 轩辕剑柒托管 OOM）：mmap(NULL) 取最低的空洞，段落进 4 GB 起那段低区——GC 段模式的新段只能落那里（坑记录 10-24 异度神剑3）。
//   开局阻塞预热（31efce0d5）1.4 秒建完 444 个着色器、段映射 796 MB / 251 块全在低区，GC 现长新段拿不到地址抛 OutOfMemoryException；同一时刻窗口下方留给宿主的 1 GB 还空着 982 MB。
//   现在每段带地址提示映射进宿主高位区（runtime/src/vawin.c s2a_host_hi_range：窗口下方的宿主保留段，或窗口之上），核落点；放不下 / 没有高位区报红一次、退回 mmap(NULL)
// 由 nvn_mtl.m 与单测各自 #include；包含方先定义 LP_LOG(fmt, ...)（一行日志，红行自带 【红】）与 LP_HI_RANGE(&lo, &hi)
//   （1 = 宿主高位区 [lo, hi)；0 = 有原生层但没有高位区，报红一次、退回低区；-1 = 没链原生层（图形单测），照旧 mmap(NULL)、不报红）。线程：库在提交线程、预热线程、预载线程上都会取，每次取各开各读（无共享可变状态）
#include "lib_pack_core.inc"
#include <errno.h>
#include <stdatomic.h>
#include "../../runtime/include/s2a_qos.h"   // 线程调度档一张表（IO_BG）；要在第一次用 s2a_qos_global 之前（11-06 原放在第 112 行，t_lib_pack 编不过、gfx 钩子红）

// 16 位十六进制键（库文件名的前 16 字符，比如 "0123456789abcdef_b"）→ 数值；不是就返回 0 并置 *ok = 0。
//   Apple 段按文件名取段用；安卓段键是 8 字节控制键原件，不走文件名（libpack_vk.c lp_key8）。
static uint64_t lp_key(const char *s, int *ok) {
    uint64_t k = 0; *ok = 1;
    for (int i = 0; i < 16; i++) { char c = s[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) { *ok = 0; return 0; } k = k << 4 | (uint64_t)v; }
    return k;
}

// 映射一段（只读文件页）：先带提示放宿主高位区，落点不在区里（区满：内核往上找到了区外）就放掉、报红一次、退回 mmap(NULL)。*hi = 1 放进了高位区。
//   释放照旧 munmap（普通文件映射，不是自己占的保留）。读数：高位区段数 / 在用字节、退回低区的段数 / 字节（nvn_mtl.m shader_load_stats 打）
static _Atomic uint64_t g_lp_hi_n, g_lp_hi_live, g_lp_low_n, g_lp_low_bytes;
static _Atomic int g_lp_hi_full_red, g_lp_hi_none_red;
static void *lp_map(size_t len, int fd, off_t at, int *hi) {
    uint64_t lo = 0, top = 0; int rc = LP_HI_RANGE(&lo, &top); *hi = 0;
    if (rc < 0) return mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, at);
    if (rc > 0) {
        void *m = top > lo && len <= top - lo ? mmap((void *)(uintptr_t)lo, len, PROT_READ, MAP_PRIVATE, fd, at) : MAP_FAILED; uint64_t a = (uint64_t)(uintptr_t)m;
        if (m != MAP_FAILED && a >= lo && a + len <= top) { *hi = 1; atomic_fetch_add(&g_lp_hi_n, 1); atomic_fetch_add(&g_lp_hi_live, len); return m; }
        if (m != MAP_FAILED) munmap(m, len);
        if (!atomic_exchange(&g_lp_hi_full_red, 1))
            LP_LOG("【红】 direct library container: the host high range %#llx-%#llx has no room for a %zu-byte segment (it landed at %#llx); this and later segments that do not fit map into the low range the GC grows its segments in (first case)",
                   (unsigned long long)lo, (unsigned long long)top, len, m == MAP_FAILED ? 0ull : (unsigned long long)a);
    } else if (!atomic_exchange(&g_lp_hi_none_red, 1))
        LP_LOG("【红】 direct library container: no host high range (no address window); library segments map into the low range the GC grows its segments in (first case)");
    void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, at);
    if (m != MAP_FAILED) { atomic_fetch_add(&g_lp_low_n, 1); atomic_fetch_add(&g_lp_low_bytes, len); }
    return m;
}
// 取一段：键 + 种类 → 分片映射的子区间（不拷贝）；没有这一段返回 NULL（不报红：很多键本来就没有 _bf / _bv）；分片缺失 / 大小不符 / 越界报红返回 NULL
static dispatch_data_t lp_get(lp_pack *p, const char *name, unsigned kind) {
    int ok; uint64_t key = lp_key(name, &ok); if (!ok || !p->idx) return NULL;
    const lp_rec *r = lp_find(p, key, kind); if (!r) return NULL;
    if (r->shard >= p->nshard) { LP_LOG("【红】 direct library container: %.16s kind %u points at shard %u of %u", name, kind, r->shard, p->nshard); return NULL; }
    if (r->off + r->len > p->ssize[2 * r->shard]) { LP_LOG("【红】 direct library container: %.16s kind %u runs past the end of shard %u", name, kind, r->shard); return NULL; }
    char path[1100]; snprintf(path, sizeof path, "%s/lib.%03u.pak", p->dir, r->shard);
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size != p->ssize[2 * r->shard]) {
        LP_LOG("【红】 direct library container: shard %s is %s (index says %llu bytes)", path, fd < 0 ? "missing" : "the wrong size", (unsigned long long)p->ssize[2 * r->shard]);
        if (fd >= 0) close(fd); return NULL; }
    const uint64_t pg = (uint64_t)getpagesize(), at = r->off & ~(pg - 1), delta = r->off - at; const size_t mlen = (size_t)(delta + (r->len ? r->len : 1));
    int hi; void *m = lp_map(mlen, fd, (off_t)at, &hi); int e = errno;
    close(fd);
    if (m == MAP_FAILED) { LP_LOG("【红】 direct library container: cannot map %.16s kind %u from %s (%u bytes at %#llx, errno %d)", name, kind, path, r->len, (unsigned long long)r->off, e); return NULL; }
    return dispatch_data_create((const uint8_t *)m + delta, r->len, s2a_qos_global(S2A_QOS_IO_BG), ^{ munmap(m, mlen); if (hi) atomic_fetch_sub(&g_lp_hi_live, mlen); });
}

#ifdef __OBJC__
// 同一段只建一个 Metal 库（11-05 时之笛）：批库（dsh_batch 编批）把上百个键的入口编进同一个 metallib，这些键的种类 0 记录指向同一段（分片 + 偏移）。
//   时之笛 41481 键只有 3234 段，1139 段被多键共用，最大一段 22 MB 被 196 个键共用。按键各自 lp_get + newLibraryWithData，同一段被映射、建库多次：
//   第十一版测试设备1 进 3D 那几秒低区被拦不到的分配吃掉约 800 MB，随后库段映射 ENOMEM（21 次，8–18 MB 的段）、着色器装不上、建 c1 缓冲失败崩溃。
//   这里按段记活着的库（弱引用：库放掉就不再记，库的生命周期照旧由持有它的着色器决定），同段的键拿同一个库；段里有没有 render_target_array_index 随段记，
//   不必每个键扫一遍整段。layer 可为 NULL。取不到返回 nil 并给 err（没有这一段：ENOENT「not in the library container」；映射失败 lp_get 已报红）
#import <Metal/Metal.h>
#include <os/lock.h>
static os_unfair_lock g_lp_lib_mu = OS_UNFAIR_LOCK_INIT;
static NSMapTable<NSString *, id<MTLLibrary>> *g_lp_libs;    // "<容器>/<分片>/<偏移>" → 活着的库
static NSMutableDictionary<NSString *, NSNumber *> *g_lp_layer;   // 同键 → 这一段用不用 render_target_array_index
static _Atomic uint64_t g_lp_lib_built, g_lp_lib_shared;    // 建了几个库 / 几次取库直接拿到同段已建好的
static id<MTLLibrary> lp_library(lp_pack *p, id<MTLDevice> dev, const char *name, unsigned kind, int *layer, NSError **err) {
    int ok; uint64_t key = lp_key(name, &ok); const lp_rec *r = ok && p->idx ? lp_find(p, key, kind) : NULL;
    if (!r) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%s is not in the library container", name]}];
              return nil; }
    NSString *bk = [NSString stringWithFormat:@"%p/%u/%llx", (void *)p->idx, r->shard, (unsigned long long)r->off];
    os_unfair_lock_lock(&g_lp_lib_mu);
    if (!g_lp_libs) { g_lp_libs = [NSMapTable strongToWeakObjectsMapTable]; g_lp_layer = [NSMutableDictionary dictionary]; }
    id<MTLLibrary> lib = [g_lp_libs objectForKey:bk]; int ly = g_lp_layer[bk].intValue;
    os_unfair_lock_unlock(&g_lp_lib_mu);
    if (lib) { atomic_fetch_add(&g_lp_lib_shared, 1); if (layer) *layer = ly; return lib; }
    dispatch_data_t d = lp_get(p, name, kind);
    if (!d) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%s: its library segment could not be mapped", name]}];
              return nil; }
    const void *b = NULL; size_t n = 0; dispatch_data_t flat = dispatch_data_create_map(d, &b, &n); (void)flat;   // one slice of one mapping: contiguous, no copy
    ly = b && memmem(b, n, "render_target_array_index", 25) != NULL;
    lib = [dev newLibraryWithData:d error:err];
    if (!lib) return nil;
    os_unfair_lock_lock(&g_lp_lib_mu);   // two threads may build the same segment at once: the first one in is kept, the other's library goes with this call
    id<MTLLibrary> cur = [g_lp_libs objectForKey:bk];
    if (cur) { lib = cur; ly = g_lp_layer[bk].intValue; } else { [g_lp_libs setObject:lib forKey:bk]; g_lp_layer[bk] = @(ly); }
    os_unfair_lock_unlock(&g_lp_lib_mu);
    atomic_fetch_add(cur ? &g_lp_lib_shared : &g_lp_lib_built, 1);
    if (layer) *layer = ly;
    return lib;
}
#endif
