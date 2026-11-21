// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include <android/log.h>
#include <hwcpipe/gpu.hpp>
#include <hwcpipe/sampler.hpp>
#include <pthread.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
// Keep the upstream MIT notice in the diagnostic native library distributed in the APK.
[[gnu::used, gnu::retain]] const char arm_license[] =
#include "mali_license.inc"
;
constexpr char tag[] = "S2AMaliCounters";
constexpr char arm_property[] = "debug.s2a.mali_arm";
constexpr char capture_property[] = "debug.s2a.mali_capture";
constexpr long long tick_ns = 250000000;
struct Metric { hwcpipe_counter id; const char *name; };
#define COUNTER(id) {id, #id}
constexpr Metric metrics[] = {
    COUNTER(MaliGPUActiveCy), COUNTER(MaliGPUAnyQueueActiveCy),
    COUNTER(MaliFragQueueActiveCy), COUNTER(MaliVertQueueActiveCy),
    COUNTER(MaliCompQueueActiveCy), COUNTER(MaliNonFragQueueActiveCy),
    COUNTER(MaliMainQueueActiveCy), COUNTER(MaliBinningQueueActiveCy),
    COUNTER(MaliExtBusRdBy), COUNTER(MaliExtBusWrBy),
    COUNTER(MaliExtBusRdStallCy), COUNTER(MaliExtBusWrStallCy),
    COUNTER(MaliExtBusRd), COUNTER(MaliExtBusWr),
    COUNTER(MaliL2CacheRdLookup), COUNTER(MaliL2CacheWrLookup),
    COUNTER(MaliSCBusTileWrBy), COUNTER(MaliFragTile), COUNTER(MaliFragTileKill),
    COUNTER(MaliSCBusFFEL2RdBy), COUNTER(MaliAnyActiveCy), COUNTER(MaliCoreActiveCy),
    COUNTER(MaliCS0WaitStallCy), COUNTER(MaliCS1WaitStallCy),
    COUNTER(MaliCS2WaitStallCy), COUNTER(MaliCS3WaitStallCy),
    COUNTER(MaliVertQueueDrainStallCy), COUNTER(MaliCompQueueDrainStallCy),
};
#undef COUNTER

bool enabled(const char *prop) {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(prop, value) == 1 && value[0] == '1';
}

long long now() {
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

bool sleep_until(long long deadline) {
    timespec t{static_cast<time_t>(deadline / 1000000000), static_cast<long>(deadline % 1000000000)};
    int rc;
    do { rc = clock_nanosleep(CLOCK_BOOTTIME, TIMER_ABSTIME, &t, nullptr); } while (rc == EINTR);
    return rc == 0;
}

void event(const char *name, int code = 0) {
    __android_log_print(ANDROID_LOG_INFO, tag,
        "{\"event\":\"%s\",\"pid\":%d,\"boot_ns\":%lld,\"code\":%d}", name, getpid(), now(), code);
}

void capture() {
    hwcpipe::gpu gpu(0);
    if (!gpu) { event("gpu_probe_failed"); return; }
    hwcpipe::sampler_config config(gpu);
    bool supported[sizeof(metrics) / sizeof(metrics[0])]{};
    std::string names;
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
        auto ec = config.add_counter(metrics[i].id);
        if (ec == hwcpipe::make_error_code(hwcpipe::errc::invalid_counter_for_device)) continue;
        if (ec) { event("config_failed", ec.value()); return; }
        supported[i] = true;
        if (!names.empty()) names += ',';
        names += '"'; names += metrics[i].name; names += '"';
    }
    if (!supported[0]) { event("gpu_active_unavailable"); return; }
    hwcpipe::sampler<> sampler(config);
    auto ec = sampler.start_sampling();
    if (ec) { event("start_failed", ec.value()); return; }
    const auto caps = gpu.get_constants();
    __android_log_print(ANDROID_LOG_INFO, tag,
        "{\"event\":\"schema\",\"pid\":%d,\"gpu_id\":%llu,\"cores\":%llu,\"l2_slices\":%llu,"
        "\"bus_width_bits\":%llu,\"counters\":[%s]}", getpid(), (unsigned long long)caps.gpu_id,
        (unsigned long long)caps.num_shader_cores, (unsigned long long)caps.num_l2_slices,
        (unsigned long long)caps.axi_bus_width, names.c_str());
    // Prime the accumulator. Each subsequent dump covers just the interval since the previous dump.
    ec = sampler.sample_now();
    if (ec) { event("prime_failed", ec.value()); }
    else {
        const long long start = now();
        long long begin = start;
        bool complete = true;
        for (int seq = 1; seq <= 120; ++seq) {
            if (!sleep_until(start + seq * tick_ns)) { event("clock_failed"); complete = false; break; }
            ec = sampler.sample_now();
            const auto end = now();
            if (ec) { event("sample_failed", ec.value()); complete = false; break; }
            std::string values;
            for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
                if (!supported[i]) continue;
                hwcpipe::counter_sample sample;
                ec = sampler.get_counter_value(metrics[i].id, sample);
                if (ec) break;
                char number[64];
                if (sample.type == hwcpipe::counter_sample::type::uint64)
                    snprintf(number, sizeof number, "%llu", (unsigned long long)sample.value.uint64);
                else if (sample.type == hwcpipe::counter_sample::type::float64 && std::isfinite(sample.value.float64))
                    snprintf(number, sizeof number, "%.17g", sample.value.float64);
                else { event("invalid_value"); complete = false; break; }
                if (!values.empty()) values += ',';
                values += number;
            }
            if (ec) { event("read_failed", ec.value()); complete = false; break; }
            if (!complete) break;
            __android_log_print(ANDROID_LOG_INFO, tag,
                "{\"event\":\"sample\",\"pid\":%d,\"seq\":%d,\"begin_ns\":%lld,\"end_ns\":%lld,\"values\":[%s]}",
                getpid(), seq, begin, end, values.c_str());
            begin = end;
        }
        if (complete) {
            ec = sampler.stop_sampling();
            if (ec) event("stop_failed", ec.value());
            else event("done");
            return;
        }
    }
    ec = sampler.stop_sampling();
    if (ec) event("stop_failed", ec.value());
}

void *wait_for_anchor(void *) {
    event("armed");
    while (enabled(arm_property)) {
        if (enabled(capture_property)) { capture(); break; }
        if (!sleep_until(now() + tick_ns)) { event("clock_failed"); break; }
    }
    return nullptr;
}
} // namespace

void s2a_mali_counters_arm() {
    if (!enabled(arm_property)) return;
    if (enabled(capture_property)) { event("stale_trigger"); return; }
    pthread_t worker;
    int rc = pthread_create(&worker, nullptr, wait_for_anchor, nullptr);
    if (rc) event("thread_failed", rc);
    else pthread_detach(worker);
}
