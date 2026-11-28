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
// 脏页追加日志（#1501 A②）：须在任何池登记前配置；失败（已有活池 / 原子操作非无锁）返回 0，调用方沿用逐页核对。
// 每次一页从只读变可写就追加一条 [a, b)（宿主页对齐地址，在页变可写之前发布）。
int nvn_dirty_log_configure(int enabled);
// 起游标：之后的事件才算。返回 0 = 这个范围没有可用日志（未启用 / 保留量用尽 / 与别的池重叠 / 换底过 / 跟踪不可靠）。
int nvn_dirty_log_open(const void *span, uint64_t *ident, uint64_t *cursor);
// 把游标之后的事件逐条交给 fn（可能重复，调用方按幂等处理），游标前移。返回 0 = 不可用或丢过事件（环绕）：
// 调用方必须按逐页代号重建自己的状态，再 nvn_dirty_log_open 重新起游标（先起游标、后读代号）。
int nvn_dirty_log_read(const void *span, uint64_t ident, uint64_t *cursor, void (*fn)(void *user, uintptr_t a, uintptr_t b), void *user);
// addr 所在页此刻只读（之后的写必然缺页、必然进日志）：1 = 是。加保护被并发的解保护挡住时为 0，调用方不能当已同步。
int nvn_dirty_page_armed(const void *span, const void *addr);
// Default-off first-access observation. Install the redirect in normal context
// with no active observation. Arm only stable, exclusive RW mappings after
// eager writeback. The caller must own mapping lifetime and cover every host
// access which bypasses userspace faults, including exported pointers.
typedef int (*nvn_dirty_read_probe_redirect_fn)(void *, void (*)(void *), void *);
void nvn_dirty_read_probe_set_redirect(nvn_dirty_read_probe_redirect_fn redirect);
void nvn_dirty_read_probe_set_guest_pc(int (*is_guest_pc)(void *context));
size_t nvn_dirty_read_probe_arm(const void *addr, size_t len);
enum nvn_defer_materialize {
    NVN_DEFER_GUEST_READ, NVN_DEFER_GUEST_WRITE, NVN_DEFER_HOST_READ, NVN_DEFER_HOST_WRITE,
    NVN_DEFER_HOST_FAULT_READ, NVN_DEFER_HOST_FAULT_WRITE, NVN_DEFER_EXPORT, NVN_DEFER_VM,
    NVN_DEFER_RANGE_CHANGE, NVN_DEFER_REPROTECT, NVN_DEFER_PREP_WRITE, NVN_DEFER_RANGE_DROP,
    NVN_DEFER_MIRROR_BORROW, NVN_DEFER_MIRROR_DROP, NVN_DEFER_GPU_WRITE, NVN_DEFER_MATERIALIZE_COUNT,
};
enum nvn_defer_reject {
    NVN_DEFER_NO_PROVIDER, NVN_DEFER_REPLAY_OPEN, NVN_DEFER_LATER_BORROW, NVN_DEFER_ALIAS_LIMIT,
    NVN_DEFER_ALLOC, NVN_DEFER_NO_HEAP, NVN_DEFER_NO_ALIAS, NVN_DEFER_BAD_SOURCE, NVN_DEFER_VM_BUSY,
    NVN_DEFER_PINNED, NVN_DEFER_HOST_ACTIVE, NVN_DEFER_NO_OWNER, NVN_DEFER_EPOCH_TABLE,
    NVN_DEFER_PAGE_STATE, NVN_DEFER_PROTECT_FAILED, NVN_DEFER_NONCOHERENT, NVN_DEFER_REJECT_COUNT,
};
struct nvn_dirty_read_probe_stats {
    uint64_t armed, read, write, host_read, host_write, censored, failed, mp_calls, mp_ns;
    uint64_t host_fault_read, host_fault_write, export_calls, export_refused_pages;
    uint64_t defer_published, defer_materialized, defer_replaced;
    uint64_t defer_by_reason[NVN_DEFER_MATERIALIZE_COUNT];
    int export_table_full;
};
void nvn_dirty_read_probe_stats(struct nvn_dirty_read_probe_stats *out);
// Pin the syscall's exact guest range against fresh read-probe protection.
// Call end after the kernel stops using it. The scope does not pin the underlying
// mapping, validate permissions or take the place of ordinary write preparation.
// The caller must keep its mapping valid through end.
struct nvn_dirty_probe_access {
    struct nvn_dirty_probe_access *next;
    uintptr_t lo, hi;
};
void nvn_dirty_read_probe_access_begin(struct nvn_dirty_probe_access *access, const void *addr,
                                       size_t len, int is_write);
void nvn_dirty_read_probe_access_end(struct nvn_dirty_probe_access *access);
// Before returning a pointer without a release contract, restore its real
// permissions and exclude all its pages from future observation. NVN pool
// removal alone is not proof that a previously returned pointer is gone.
void nvn_dirty_read_probe_pin_export(const void *addr, size_t len, int is_write);
// The VM owner must begin before parking other threads and end after
// changing backing or permissions. A nonzero token requires a matching end.
int nvn_dirty_read_probe_vm_begin(void);
void nvn_dirty_read_probe_vm_end(void);
// A completed immutable source for whole pages. The producer owns the slot
// and a private RW alias of destination until remaining reaches zero. The
// alias is never exported to ordinary readers. No Vulkan call is made here.
struct nvn_dirty_deferred_source {
    const void *destination;
    void *alias;
    const void *completed_bytes;
    size_t length;
    uint64_t version;
    size_t remaining;
    int replace_verified;   // proof of complete overwrite, separate from successful completion
    enum nvn_defer_reject reject;   // first closed gate on a failed install
    const void *page_slots;   // private to nvn_dirty, lives until the session is drained
};
// Completion proves host visibility. A replacement separately requires
// complete overwrite, a newer version and ownership of every covered page.
size_t nvn_dirty_land_defer_install(struct nvn_dirty_deferred_source *source, int completed);
size_t nvn_dirty_land_defer_remaining(const struct nvn_dirty_deferred_source *source);
int nvn_dirty_land_defer_owns_page(const struct nvn_dirty_deferred_source *source, uintptr_t address);
// Normal context before the caller reuses or releases any producer storage.
int nvn_dirty_land_defer_flush(struct nvn_dirty_deferred_source *source, enum nvn_defer_materialize reason);
int nvn_dirty_land_defer_flush_range(struct nvn_dirty_deferred_source *source, uintptr_t addr, size_t len,
                                    enum nvn_defer_materialize reason);
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
