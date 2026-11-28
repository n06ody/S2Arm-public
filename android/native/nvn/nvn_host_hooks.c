// Android 生产目标：把共享前端/直连层用到的宿主名字（dm_*，graphics/nvn/host/dm.h）接到本线的真实 dispatcher。
// 之前生产链接里只有 vk_engine_host_shim.c 的弱空实现，nvn_dirty 的缺页钩子和写前通知都落空。
// 这里只登记，不安装任何信号处理器：缺页仍由 crash.c 的 on_crash 先问 s2a_direct_fault；
// 写前通知挂在 graphics/track 的唯一 prep 回调上（s2a_guest_write_prep_register 已转发给 track）。
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "s2a_direct_host.h"
#include "s2a_track.h"
#include "graphics/nvn/host/dm.h"
#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
#include "../runtime/fault/fault.h"
#include "../runtime/vm/backing.h"
#include "../../../platform/plat.h"
#include <sys/mman.h>
#include <sys/ucontext.h>
#include "graphics/nvn/gfx/nvn/nvn_dirty.h"
#include "graphics/nvn/replay/nvn_plat.h"
#endif

// dm 侧回调签名 (void *, size_t)，direct 侧登记的是 (uint64_t, uint64_t)：桥接保持两侧 ABI 不变。
static _Atomic(void (*)(void *, size_t)) g_dm_prep;

static void prep_bridge(uint64_t addr, uint64_t len) {
    void (*fn)(void *, size_t) = atomic_load_explicit(&g_dm_prep, memory_order_acquire);
    if (fn) fn((void *)(uintptr_t)addr, (size_t)len);
}

void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uctx)) {
    s2a_fault_hook_register(fn);
}

void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (!fn) return;
    atomic_store_explicit(&g_dm_prep, fn, memory_order_release);
    s2a_guest_write_prep_register(prep_bridge);
}

void dm_guest_write_prepare(void *addr, size_t len) {
    if (!addr || !len) return;
    s2a_track_write_quiet((uint64_t)(uintptr_t)addr, (uint64_t)len);
}

typedef void (*nvn_land_pin_fn)(uint64_t addr, uint64_t len, int is_write);

#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
static int probe_guest_pc(void *context) {
    const ucontext_t *u = context;
    return u && module_of((const void *)(uintptr_t)u->uc_mcontext.pc) != NULL;
}
static void pin_host_address(uint64_t addr, uint64_t len, int is_write) {
    nvn_dirty_read_probe_pin_export((const void *)(uintptr_t)addr, (size_t)len, is_write);
}
#endif

// AndroidGameRuntime binds this before constructing HLE. The native provider
// does not arm pages until that caller has had a chance to record exports.
nvn_land_pin_fn s2a_android_nvn_land_pin_provider(void) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
    if (nvn_plat_flag("nvk_land_read_probe", 0) == 1 || nvn_plat_flag("nvk_land_defer_cs", 0) == 1) {
        nvn_dirty_read_probe_set_guest_pc(probe_guest_pc);
        nvn_dirty_read_probe_set_redirect(s2a_android_fault_redirect_call);
        return pin_host_address;
    }
#endif
    return NULL;
}

#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
static size_t arm_land_probe(const void *addr, size_t len, void *opaque) {
    (void)opaque;
    return nvn_dirty_read_probe_arm(addr, len);
}

static size_t install_deferred(const void *addr, size_t len, void *opaque) {
    struct nvn_dirty_deferred_source *source = opaque;
    source->reject = NVN_DEFER_BAD_SOURCE;
    if (addr != source->destination || len != source->length) return 0;
    source->reject = NVN_DEFER_NO_ALIAS;
    void *alias = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (alias == MAP_FAILED) return 0;
    // The VM mutation gate materializes these pages before replacing their
    // backing. This private alias never escapes to a host or guest consumer.
    if (!plat_backing_map(alias, len, PLAT_PROT_READ | PLAT_PROT_WRITE, (uintptr_t)addr)) {
        source->alias = alias;
        const size_t installed = nvn_dirty_land_defer_install(source, 1);
        if (installed) return installed;
        source->alias = NULL;
    }
    if (munmap(alias, len)) die("nvk deferred writeback: cannot discard private alias");
    return 0;
}
#endif

size_t rp_platform_land_probe_arm(const void *addr, size_t len) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
    if (s2a_android_fault_dispatch_ready())
        return s2a_android_mem_land_probe_heap(addr, len, arm_land_probe, NULL);
#else
    (void)addr;
    (void)len;
#endif
    return 0;
}

struct nvn_dirty_deferred_source;
size_t rp_platform_land_defer_install(struct nvn_dirty_deferred_source *source) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
    source->reject = NVN_DEFER_NO_PROVIDER;
    if (s2a_android_fault_dispatch_ready()) {
        source->reject = NVN_DEFER_NO_HEAP;
        return s2a_android_mem_land_probe_heap(source->destination, source->length, install_deferred, source);
    }
#else
    (void)source;
#endif
    return 0;
}

void rp_platform_land_defer_release(struct nvn_dirty_deferred_source *source) {
#if defined(__ANDROID__) && defined(S2A_ANDROID_GAME_RUNTIME) && defined(S2A_ANDROID_DEBUG_ENV)
    if (munmap(source->alias, source->length)) die("nvk deferred writeback: cannot retire private alias");
#else
    (void)source;
#endif
}
