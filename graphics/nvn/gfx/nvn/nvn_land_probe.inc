// SPDX-License-Identifier: Apache-2.0
// g_slot_mu serializes arm, retirement and range lifecycle. A protected page holds its
// slot number. The fault handler must acquire that exact state before looking at the
// immutable sample; the ordinary dirty tracker gets the page back before the hook returns.
enum { PROBE_BASE = 4, PROBE_LIMIT = 128 };
_Static_assert(PROBE_BASE + PROBE_LIMIT <= UINT8_MAX, "probe state fits");
// Keep proof for a delayed concurrent fault after a window has drained. A slot is
// never recycled for another page in this session; a full table declines new pages.
enum { RETRY_SLOTS = 4096 };
struct probe_retry { _Atomic uintptr_t page; _Atomic uint64_t life, write; };
static struct probe_retry retry_table[RETRY_SLOTS];
static uint64_t next_range_life;
static struct probe_retry *probe_retry_entry(uintptr_t page, int reserve) {
    size_t h = ((page >> g_shift) * UINT64_C(11400714819323198485)) % RETRY_SLOTS;
    for (int i = 0; i < 8; i++) {
        struct probe_retry *e = &retry_table[(h + i) % RETRY_SLOTS];
        uintptr_t key = atomic_load(&e->page);
        if (key == page) return e;
        if (!key) {
            if (!reserve) return NULL;
            atomic_store(&e->page, page); return e;
        }
    }
    return NULL;
}
struct probe_slot {
    struct nvn_probe_sample sample;
    uintptr_t lo, hi, page;
    range_t *owner;
    struct probe_retry *retry;
    uint8_t previous;
    _Atomic unsigned result;
};
static struct probe_slot probe_slots[PROBE_LIMIT];
static unsigned probe_n;
static _Atomic int probe_ever;
static uint64_t probe_dropped;
static _Thread_local uintptr_t probe_last_retry;
struct probe_rotation { uint64_t pool, offset, len, turn; };
static struct probe_rotation rotations[PROBE_LIMIT];

#if defined(__ANDROID__) || defined(S2A_LAND_PROBE_TEST)
int nvn_probe_guest_context(void *ctx) __attribute__((weak));
#endif
static int probe_has_classifier(void) {
#if defined(__ANDROID__) || defined(S2A_LAND_PROBE_TEST)
    return nvn_probe_guest_context != NULL;
#else
    return 0;
#endif
}
static int probe_classify(void *ctx) {
#if defined(__ANDROID__) || defined(S2A_LAND_PROBE_TEST)
    if (ctx && nvn_probe_guest_context) return nvn_probe_guest_context(ctx);
#else
    (void)ctx;
#endif
    return -1;
}

// 1 = restored, 0 = another thread claimed it, -1 = the OS refused restoration.
static int probe_release(range_t *r, uintptr_t k, uint8_t state, unsigned reason, uintptr_t addr, void *ctx) {
    if (state < PROBE_BASE || !atomic_compare_exchange_strong(&r->state[k], &state, PG_BUSY)) return 0;
    struct probe_slot *s = &probe_slots[state - PROBE_BASE];
    if (!reason || reason == NVN_PROBE_WRITE_FIRST) {
        if (addr < s->lo || addr >= s->hi) reason = NVN_PROBE_NEIGHBOR;
        else if (!reason) {
            int guest = probe_classify(ctx);
            reason = guest < 0 ? NVN_PROBE_UNAVAILABLE : guest ? NVN_PROBE_GUEST_READ : NVN_PROBE_HOST_READ;
        }
    }
    if (mprotect((void *)s->page, g_pg, s->previous == PG_PROT ? PROT_READ : PROT_READ | PROT_WRITE) != 0) {
        atomic_store(&g_track_unreliable, 1);
        atomic_store(&r->state[k], state);
        return -1;
    }
    atomic_store(&s->retry->life, atomic_load(&r->probe_life));
    atomic_store(&s->retry->write, s->sample.write);
    atomic_store(&r->state[k], s->previous);
    atomic_store(&s->result, reason); // final publication: the slot can now be recycled
    return 1;
}
static int probe_retire(unsigned i, unsigned reason) {
    struct probe_slot *s = &probe_slots[i];
    uint64_t started = 0;
    for (unsigned spin = 1; !atomic_load(&s->result); spin++) {
        uintptr_t k = (s->page - s->owner->base) >> g_shift;
        if (!busy_wait(s->owner, k)) return 0;
        if (probe_release(s->owner, k, PROBE_BASE + i, reason, 0, NULL) < 0) return 0;
        // The winner publishes the terminal reason after restoring the page state.
        if (!(spin & 1023)) {
            uint64_t now = dirty_tick();
            if (!started) started = now;
            if ((now - started) * g_tb_num / g_tb_den > (uint64_t)BUSY_WAIT_MS * 1000000) return 0;
            sched_yield();
        }
    }
    return 1;
}
static void probe_retire_range(uintptr_t lo, uintptr_t hi, uint64_t pool, uint64_t write) {
    for (unsigned i = 0; i < probe_n; i++) {
        struct probe_slot *s = &probe_slots[i];
        if (atomic_load(&s->result)) continue;
        int superseded = pool && s->sample.pool == pool && write > s->sample.write && lo < s->hi && s->lo < hi;
        if (!superseded && (s->page >= hi || lo >= s->page + g_pg)) continue;
        probe_retire(i, superseded ? NVN_PROBE_NEXT_VERSION : NVN_PROBE_ALIAS);
    }
}
void nvn_probe_before(const void *mem, size_t len, uint64_t pool, uint64_t write) {
    uintptr_t a = (uintptr_t)mem;
    if (!len || len > UINTPTR_MAX - a || !atomic_load(&probe_ever)) return;
    pthread_mutex_lock(&g_slot_mu);
    probe_retire_range(a, a + len, pool, write);
    pthread_mutex_unlock(&g_slot_mu);
}
static uint64_t probe_turn(uint64_t pool, uint64_t off, uint64_t len) {
    size_t start = (pool ^ (off >> 5) ^ len) % PROBE_LIMIT;
    for (size_t j = 0; j < PROBE_LIMIT; j++) {
        struct probe_rotation *r = &rotations[(start + j) % PROBE_LIMIT];
        if (!r->pool) *r = (struct probe_rotation){pool, off, len, 0};
        if (r->pool == pool && r->offset == off && r->len == len) return r->turn++;
    }
    rotations[start] = (struct probe_rotation){pool, off, len, 1};
    return 0;
}
static uint64_t probe_page_index(uint64_t turn, uint64_t id, uint64_t n) {
    if (turn % 4 == 0) return 0;
    if (turn % 4 == 1) return n / 2;
    if (turn % 4 == 2) return n - 1;
    id ^= id >> 30; id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27; id *= UINT64_C(0x94d049bb133111eb);
    return (id ^ (id >> 31)) % n;
}
void nvn_probe_after(const void *mem, size_t len, uint64_t off, uint64_t pool, uint64_t write, int overlap, struct nvn_probe_source source) {
    uintptr_t a = (uintptr_t)mem;
    pthread_mutex_lock(&g_slot_mu);
    if (probe_n == PROBE_LIMIT) { probe_dropped++; goto out; }
    unsigned i = probe_n++;
    struct probe_slot *s = &probe_slots[i];
    s->sample = (struct nvn_probe_sample){.source=source, .pool=pool, .write=write, .offset=off, .bytes=len};
    atomic_store(&s->result, NVN_PROBE_UNAVAILABLE);
    if (!probe_has_classifier() || !g_pg || !len || len > UINTPTR_MAX - a || !pool || !write || atomic_load(&g_track_unreliable)) goto out;
    uintptr_t first = a & ~(g_pg - 1);
    s->sample.pages = 1 + ((a + len - 1 - first) >> g_shift);
    s->sample.page_index = probe_page_index(probe_turn(pool, off, len), write, s->sample.pages);
    s->page = first + s->sample.page_index * g_pg;
    s->lo = a; s->hi = a + len;
    if (overlap) { atomic_store(&s->result, NVN_PROBE_ALIAS); goto out; }
    range_t *r = NULL;
    for (int j = 0; j < atomic_load(&g_nr); j++) {
        range_t *other = rg(j);
        if (!live_at(j) || s->page < other->base || s->page >= other->end) continue;
        if (r) { atomic_store(&s->result, NVN_PROBE_ALIAS); goto out; }
        r = other;
    }
    if (!r || off > a || r->mem != a - off || s->hi > r->end) goto out;
    uintptr_t k = (s->page - r->base) >> g_shift;
    uint8_t st = atomic_load(&r->state[k]);
    if (st >= PROBE_BASE) { atomic_store(&s->result, NVN_PROBE_ALIAS); goto out; }
    s->retry = probe_retry_entry(s->page, 1);
    if (!s->retry) goto out;
    if ((st != PG_PROT && st != PG_NONE) || !atomic_compare_exchange_strong(&r->state[k], &st, PG_BUSY)) goto out;
    s->owner = r; s->previous = st;
    atomic_store(&probe_ever, 1);
    if (mprotect((void *)s->page, g_pg, PROT_NONE) != 0) { atomic_store(&r->state[k], st); goto out; }
    s->sample.armed = 1;
    atomic_store(&s->result, 0);
    atomic_store(&r->state[k], PROBE_BASE + i);
out:
    pthread_mutex_unlock(&g_slot_mu);
}
uint64_t nvn_probe_drain(nvn_probe_emit emit) {
    pthread_mutex_lock(&g_slot_mu);
    for (unsigned i = 0; i < probe_n; i++) {
        if (!probe_retire(i, NVN_PROBE_EXPIRED)) { pthread_mutex_unlock(&g_slot_mu); return UINT64_MAX; }
    }
    for (unsigned i = 0; i < probe_n; i++) {
        probe_slots[i].sample.result = atomic_load(&probe_slots[i].result);
        if (emit) emit(&probe_slots[i].sample);
    }
    uint64_t dropped = probe_dropped;
    probe_dropped = probe_n = 0;
    pthread_mutex_unlock(&g_slot_mu);
    return dropped;
}
