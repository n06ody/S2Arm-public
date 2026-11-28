// Android 背板（memfd）核心接口。单一 memfd、pgoff 等于 guest 地址、
// 打洞按宿主页；失败一律回正 errno，由调用方决定终止，不提供匿名映射回退。
//
// 本头只属于 Android 组合层（android/native），不进 platform/plat.h，
// 苹果编译单元看不到这些符号。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 建立或扩大背板。limit 按宿主页向上取整，只增不缩；0 = 尚未 open。
int plat_backing_open(uint64_t limit);
// 把 [pgoff, pgoff+size) 以 MAP_SHARED|MAP_FIXED 映射到 addr，prot 用 PLAT_PROT_*。
int plat_backing_map(void *addr, size_t size, uint32_t plat_prot, uint64_t pgoff);
// 区间内任一页 refs>0 时 *deferred=1、不调内核、回 0；否则打洞并置 *deferred=0。
// 返回值不再兼作 deferred 标志。deferred 必须非空。
int plat_backing_punch(uint64_t pgoff, size_t size, int *deferred);
// 稀疏区间引用表：按宿主页记数，升序、不重叠、相邻同数合并。
int plat_backing_ref(uint64_t pgoff, size_t size);
int plat_backing_unref(uint64_t pgoff, size_t size);   // 下溢或区间不匹配回 EINVAL
uint32_t plat_backing_refs(uint64_t pgoff, size_t size);   // 区间内最大引用数；未 open /
                                                           // 未对齐 / 越界回 UINT32_MAX（fail-closed）
// 打洞世代号：每次真正 fallocate 前后各 +1，奇数表示正在打洞。
uint64_t plat_backing_sequence(void);
// 会话复位 / 测试 setup：refs 非空回 EBUSY；否则整文件打洞。
int plat_backing_reset(void);
uint64_t plat_backing_limit(void);      // 0 = 尚未 open
uint64_t plat_backing_deferred(void);   // 推迟打洞的累计次数

// svc_mem.c 提供给 NVN 长寿别名（#1281 稀疏虚拟池）的入口，只在 Android 定义。
// ref: 1 = 已 ref；0 = 源不在背板区（宿主内存）；-EFAULT = 源页已死；其它 <0 = -errno。
int  s2a_mem_backing_alias_ref(uint64_t src, uint64_t n);
// unref 后若源已死就按宿主页打洞（此时页已无任何别名引用）。
void s2a_mem_backing_alias_unref(uint64_t src, uint64_t n);
// Visit a page-aligned, private RW heap extent under the VM owner lock. The
// visitor must never reenter the VM owner. Zero means the proof was unavailable.
size_t s2a_android_mem_land_probe_heap(const void *addr, size_t len,
                                       size_t (*visit)(const void *, size_t, void *), void *opaque);

#ifdef __cplusplus
}
#endif
