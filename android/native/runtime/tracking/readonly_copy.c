#include "readonly_copy.h"
#include "../../../../runtime/include/s2a_native.h"
#include "../../../../graphics/track/track_platform.h"
#include "../../../../platform/plat.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <sched.h>
#if defined(__linux__)
#include "mirror.h"
#include "../../../../graphics/nvn/replay/rp_guest_snapshot.h"
int rp_buf_guest_snapshot(uint64_t, void *, size_t, enum rp_land_reader) __attribute__((weak));
#include "../vm/backing.h"
static uint64_t readonly_backing_sequence(void) { return plat_backing_sequence(); }
#else
static uint64_t s2a_android_mirror_sequence(void) { return 0; }
static void s2a_android_mirror_note_retry(void) {}
static void s2a_android_mirror_note_wait(void) {}
static uint64_t readonly_backing_sequence(void) { return 0; }
#endif

#ifdef S2A_UNIT_TEST
void (*s2a_android_readonly_test_hook)(void);
#endif

// Low bit: bounded check; high bit: first-copy freeze. No per-copy getenv/log IO.
static _Atomic int check_mode = 1;
int s2a_android_readonly_configure_bounded(int enabled) {
    if (enabled != 0 && enabled != 1) return EINVAL;
    int mode = atomic_load_explicit(&check_mode, memory_order_relaxed);
    for (;;) {
        if (mode & 2) return (mode & 1) == enabled ? 0 : EBUSY;
        if (atomic_compare_exchange_weak_explicit(&check_mode, &mode, enabled,
                memory_order_relaxed, memory_order_relaxed)) return 0;
    }
}

int s2a_android_copy_guest_readonly(uint64_t address, void *destination, uint64_t size) {
    if (!size) return 0;
    uintptr_t target = (uintptr_t)destination;
    size_t page = plat_page_size();
    if (!destination || !page || (page & (page - 1)) || size > SIZE_MAX
        || size > UINT64_MAX - address || size > UINTPTR_MAX - target) return EINVAL;
    uint64_t end = address + size;
    if (address < target + size && target < end) return EINVAL;

    // Validation and all copied pages must belong to the same VM generation.
    // Never hold g_mu or tracker locks while waiting for a writer or copying.
    // 重试有界（同 track.c S2A_ALIAS_STALE_MAX 口径）：持续 VM 事务下无界自旋是活锁；
    // 耗尽返回 EFAULT——不静默、不零填、调用方按错误处理（#1184 复核 ⑧）。
    int bounded = atomic_fetch_or_explicit(&check_mode, 2, memory_order_relaxed) & 1;
    for (int attempts = 0;;) {
        uint64_t seq=s2a_android_mirror_sequence();
        // 背板打洞世代号（#1217 §2.6）：镜像关时 mirror_sequence 恒 0，0x2D / 缩堆在事务里
        // fallocate 会清掉别名还要读的页；与 :51 同一位置读，奇数（正在打洞）让出 CPU 重读。
        uint64_t bseq=readonly_backing_sequence();
        if (bseq&1) { sched_yield(); continue; }
        if (seq&1) {   // 等换页事务结束：有界；超时不当「不可读」静默返回
#if defined(__linux__)
            if (s2a_track_platform_wait_stable()) {
                fprintf(stderr, "[track] 【红】readonly copy 等待换页世代超时，返回 EAGAIN（不是页不可读）\n");
                return EAGAIN;
            }
#else
            s2a_android_mirror_note_wait(); sched_yield();
#endif
            continue;
        }
        int error=0;
        if (bounded) {
            if (!s2a_mem_readable(address,size)) error=EFAULT;
        } else {
            for (uint64_t cursor=address;cursor<end;) {
                s2a_meminfo info;
                error=s2a_mem_query(cursor,&info);
                if (error) break;
                if (!(info.perm&1) || info.base>cursor || !info.size ||
                    info.size>UINT64_MAX-info.base || info.base+info.size<=cursor) { error=EFAULT; break; }
                uint64_t next=info.base+info.size;
                cursor=next<end ? next : end;
            }
        }
        int snapshot = 0;
#if defined(__linux__)
        if (!error && rp_buf_guest_snapshot) {
            const int guarded = rp_buf_guest_snapshot(address, destination, (size_t)size, RP_LAND_READ_HOST);
            if (guarded != ENOSYS) { error = guarded; snapshot = 1; }
        }
#endif
        uint8_t *output=destination;
        for (uint64_t cursor=address;!snapshot && !error && cursor<end;) {
            uint64_t base=cursor & ~((uint64_t)page-1);
            if (base>UINTPTR_MAX-page) { error=EINVAL; break; }
            size_t offset=(size_t)(cursor-base), count=page-offset;
            if (count>end-cursor) count=(size_t)(end-cursor);
            void *alias=NULL;
            error=s2a_track_platform_alias((uintptr_t)base,page,PROT_READ,&alias);
            if (error) break;
            memcpy(output,(const uint8_t *)alias+offset,count);
            error=s2a_track_platform_unalias(alias,page);
            output+=count; cursor+=count;
#ifdef S2A_UNIT_TEST
            if (!error && s2a_android_readonly_test_hook) s2a_android_readonly_test_hook();
#endif
        }
        atomic_thread_fence(memory_order_seq_cst);
        uint64_t end_seq=s2a_android_mirror_sequence();
        uint64_t bend=readonly_backing_sequence();
        if (seq!=end_seq || (end_seq&1) || bseq!=bend || (bend&1) || error==ESTALE) {
            s2a_android_mirror_note_retry();
            if (++attempts > 8) {
                // 与「地址范围不可读」（EFAULT）区分：这是世代搅动重试耗尽，调用方按可重试处理。
                fprintf(stderr, "[track] 【红】readonly copy 世代重试耗尽（%d 次），返回 EAGAIN（不是页不可读）\n", attempts);
                return EAGAIN;
            }
            continue;
        }
        return error;
    }
}
