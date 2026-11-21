// 配方级在途表（纯逻辑）。合同见 nvn_pipe_flight.h。
#include "nvn_pipe_flight.h"

#include <string.h>

// 首次进入时在锁内做一遍初始化（pthread_once 的回调不带参数，每个 nvk_pf 各自一份 once 反而要
// 「全局登记正在初始化的表」；这把锁本来就护整张表，第一次初始化也放在这里，语义相同且无全局状态）。
static void pf_ensure(nvk_pf *t) {
    if (t->ready) return;
    if (t->key_bytes == 0 || t->key_bytes > NVK_PF_KEY_MAX) return;   // 停用
    int ok = 1;
    for (int i = 0; i < NVK_PF_MAX; i++)
        if (pthread_cond_init(&t->ent[i].cv, NULL) != 0) { ok = 0; break; }
    t->ready = ok;
}
void nvk_pf_init(nvk_pf *t, size_t key_bytes) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    t->key_bytes = key_bytes;
    pthread_mutex_lock(&t->mu);
    pf_ensure(t);
    pthread_mutex_unlock(&t->mu);
}
// 调用方持 t->mu
static struct nvk_pf_ent *pf_find_locked(nvk_pf *t, const void *key, int (*eq)(const void *, const void *)) {
    for (int i = 0; i < NVK_PF_MAX; i++)
        if (t->ent[i].used && eq(t->ent[i].key, key)) return &t->ent[i];
    return NULL;
}
static int pf_index(nvk_pf *t, struct nvk_pf_ent *e) { return (int)(e - t->ent); }
int nvk_pf_enter(nvk_pf *t, const void *key, int (*eq)(const void *, const void *), int *handle) {
    if (!t || !key || !eq || !handle) return 0;
    *handle = -1;
    pthread_mutex_lock(&t->mu);
    pf_ensure(t);
    if (!t->ready) { t->full++; pthread_mutex_unlock(&t->mu); return 0; }
    struct nvk_pf_ent *e = pf_find_locked(t, key, eq);
    if (e) { e->users++; t->waits++; *handle = pf_index(t, e); pthread_mutex_unlock(&t->mu); return 2; }
    for (int i = 0; i < NVK_PF_MAX; i++) {
        struct nvk_pf_ent *c = &t->ent[i];
        if (c->used) continue;
        c->used = 1; c->users = 1; c->done = 0; c->obj = NULL; c->code = 0; c->why[0] = 0;
        memcpy(c->key, key, t->key_bytes);
        *handle = i;
        pthread_mutex_unlock(&t->mu);
        return 1;
    }
    t->full++;   // 表满：这一次不合并
    pthread_mutex_unlock(&t->mu);
    return 0;
}
void nvk_pf_publish(nvk_pf *t, int handle, void *obj, int code, const char *why) {
    if (!t || handle < 0 || handle >= NVK_PF_MAX) return;
    pthread_mutex_lock(&t->mu);
    struct nvk_pf_ent *e = &t->ent[handle];
    if (e->used) {
        e->obj = obj; e->code = code;
        if (why && code) { strncpy(e->why, why, sizeof e->why - 1); e->why[sizeof e->why - 1] = 0; }
        e->done = 1;
        if (code) t->failures++; else t->published++;
        pthread_cond_broadcast(&e->cv);
        if (--e->users == 0) { e->used = 0; e->done = 0; e->obj = NULL; e->why[0] = 0; }
    }
    pthread_mutex_unlock(&t->mu);
}
int nvk_pf_wait(nvk_pf *t, int handle, void **obj, char *why, size_t why_cap) {
    if (!t || handle < 0 || handle >= NVK_PF_MAX || !obj) return -1;
    pthread_mutex_lock(&t->mu);
    struct nvk_pf_ent *e = &t->ent[handle];
    while (e->used && !e->done) pthread_cond_wait(&e->cv, &t->mu);
    int code = e->used ? e->code : 0;
    void *got = e->used ? e->obj : NULL;
    if (e->used && why && why_cap) { strncpy(why, e->why, why_cap - 1); why[why_cap - 1] = 0; }
    // 成功就必须有对象：(NULL,0) 交付给等待侧就是「成功拿到空管线」——调用方会把 NULL 当对象往下用，
    //   成员偏移解引即崩（真实 0x28 崩溃链：pipe->dsl[2] 正在 +0x28）。两类来源都堵在这：
    //   建者错误地发布 (NULL,0)，或表项在放行前被复用（e->used==0 且无 code）。
    if (!got && !code) {
        code = -1;
        if (why && why_cap) { const char *m = "管线在途表交付了空对象"; strncpy(why, m, why_cap - 1); why[why_cap - 1] = 0; }
    }
    if (e->used && --e->users == 0) { e->used = 0; e->done = 0; e->obj = NULL; e->why[0] = 0; }
    pthread_mutex_unlock(&t->mu);
    *obj = got;
    return code;
}
void nvk_pf_note_late_hit(nvk_pf *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    t->late_hit++;
    pthread_mutex_unlock(&t->mu);
}
void nvk_pf_stats(const nvk_pf *t, uint64_t *waits, uint64_t *full, uint64_t *late_hit,
                  uint64_t *published, uint64_t *failures) {
    if (!t) return;
    nvk_pf *m = (nvk_pf *)t;
    pthread_mutex_lock(&m->mu);
    if (waits) *waits = t->waits;
    if (full) *full = t->full;
    if (late_hit) *late_hit = t->late_hit;
    if (published) *published = t->published;
    if (failures) *failures = t->failures;
    pthread_mutex_unlock(&m->mu);
}
