// Optional CPU-side measurements for the Vulkan replay backend.
#pragma once
#include "nvn_backend.h"
#include <errno.h>
#include <pthread.h>

// 弱链接登记在 rp_res.c，宿主和未提供诊断的后端沿用原步序。
#pragma weak nvn_gpu_idle_observer
static inline const struct nvn_gpu_idle_observer *rp_idle_observer(void) {
    return nvn_gpu_idle_observer ? nvn_gpu_idle_observer() : NULL;
}

static inline void rp_idle_lock(pthread_mutex_t *mu, enum nvn_gpu_idle_site site) {
    const struct nvn_gpu_idle_observer *o = rp_idle_observer();
    if (o) {
        int rc = pthread_mutex_trylock(mu);
        if (!rc) return;
        if (rc == EBUSY) {
            uint64_t begin = o->clock_ns();
            pthread_mutex_lock(mu);
            o->note(site, NULL, NULL, 0, o->clock_ns() - begin);
            return;
        }
    }
    pthread_mutex_lock(mu);
}
