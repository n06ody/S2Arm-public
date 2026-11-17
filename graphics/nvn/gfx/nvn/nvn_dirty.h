// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

extern int (*nvn_dirty_far_fn)(uint64_t a, uint64_t n);          // 远端别名区判定（宿主填）：登记时远端池不登记、不加写保护
void nvn_dirty_add_range(const void *mem, size_t size);           // Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // Set the covered pages read-only
// 保护分块（页数，相对所属映射起点对齐）。0 = 逐页（共享默认，与 main 一致；Apple 路径也编本文件）。
// 安卓直连在第一次纹理上传前设 1 MiB（debug.s2a.chunk_kb 可调），避免逐页 mprotect 切碎 AHB 映射。
// 换底（AHB 换页）后重建跟踪：原地重置状态、发布新映射代际、按分块重新加保护。
// 返回 1 = 已替换；0 = 该地址本来没有跟踪条目（调用方应走 add_range）。
int nvn_dirty_replace_range(const void *mem, size_t size);
// 映射代际：每次 add/remove/replace 后变化，缓存据此重新验证句柄与保护状态。
uint64_t nvn_dirty_mapping_generation(void);
// 可选的 64 宿主页摘要：须在任何池登记前配置；失败时调用方沿用逐页检查。
// 块以宿主页的绝对地址对齐，与 mprotect 分块大小无关。
int nvn_dirty_blocks_configure(int enabled);
// 对一个摘要块内的范围取稳定快照；返回 1 时 version + epoch 可以作为缓存身份。
// 未跟踪、跨块、写入进行中、别名重叠或跟踪不可靠均返回 0，调用方必须保守处理。
int nvn_dirty_block_snapshot(const void *span, const void *addr, size_t len, uint64_t *version, uint64_t *epoch);
void nvn_dirty_set_chunk_pages(uint32_t pages);
uint32_t nvn_dirty_chunk_pages(void);
// 跟踪不可靠：任一 mprotect 失败后置位；为 1 时所有查询走保守路径（视为脏 / 代际 -1）。
int nvn_dirty_tracking_unreliable(void);
uint64_t nvn_dirty_now(void);                                      // Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // Covered pages were written after the stamp -> 1
const void *nvn_dirty_span(const void *addr, size_t len);          // The tracked range holding the span (a handle to keep; NULL = untracked)
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);   // nvn_dirty_since through a kept handle (rechecked, scanned again only if it no longer holds the span)
long nvn_dirty_page_gens(const void *span, const void *addr, size_t len, uint64_t *gens, size_t n);   // write generation of each page of the span (0 = never written); page count, -1 = untracked / over n
// 取范围（find_span）走有序索引，答案与线性扫逐位相同（装得下的范围里槽号最小的），只是不再每问一次扫一遍整张表。
// 调用线程自己的累计：索引答的次数、退回线性扫的次数（写者正在改表 / 复查代际不一致 / 零长度 / 索引不可用）、索引答的那些次一共访问了多少个节点。
void nvn_dirty_span_stats(uint64_t *idx_hits, uint64_t *scans, uint64_t *nodes);
size_t nvn_dirty_page_size(void);                                  // Host page size the tracking works in (16 KB on Apple systems: one page can hold several 4 KB-aligned NVN resources)
long nvn_dirty_written_pages(const void *span, const void *addr, size_t len, uint64_t stamp, uint64_t *bits, size_t nbits);   // bit k = page k of the span written after stamp; returns the page count, -1 = untracked / over nbits
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // Currently live slots, slot table high-water mark, total number of registrations
