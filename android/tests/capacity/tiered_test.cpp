// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE 1
#include "../../native/capacity_affinity.h"
#include "../../../graphics/nvn/replay/nvn_plat.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

static int tiered_flag = 1;
static cpu_set_t allowed, replay, recorder, remaining;
static pthread_barrier_t active;
static bool bind_done_background;

static void completion_startup() {
#define __ANDROID__ 1
#include "vk_done_startup.inc"
#undef __ANDROID__
    if (bind_done_background) s2a_capacity_enter(S2A_CAPACITY_BACKGROUND);
}

extern "C" int nvn_plat_flag(const char *name, int dflt) {
    assert(!std::strcmp(name, "nvk_hot_cores") && dflt == 1);
    return tiered_flag;
}

static cpu_set_t mask(std::initializer_list<int> cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus) CPU_SET(cpu, &set);
    return set;
}

static cpu_set_t read_mask(pid_t tid) {
    cpu_set_t actual;
    CPU_ZERO(&actual);
    assert(!sched_getaffinity(tid, sizeof(actual), &actual));
    return actual;
}

static void *wide_child(void *) {
    cpu_set_t actual = read_mask(0);
    assert(CPU_EQUAL(&actual, &allowed));
    return nullptr;
}

struct Worker {
    void (*enter)();
    pid_t tid = 0;
};

static void rendezvous() {
    int rc = pthread_barrier_wait(&active);
    assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *worker_main(void *arg) {
    auto &worker = *static_cast<Worker *>(arg);
    if (worker.enter) worker.enter();
    worker.tid = static_cast<pid_t>(syscall(SYS_gettid));
    rendezvous();
    rendezvous();
    return nullptr; // The production worker TLS handles removal.
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const bool omit_audio = argc == 3 && !std::strcmp(argv[2], "--omit-audio-role");
    bind_done_background = argc == 3 && !std::strcmp(argv[2], "--bind-done-background");
    auto original = read_mask(0);
    int cpus[5], count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && count < 5; ++cpu)
        if (CPU_ISSET(cpu, &original)) cpus[count++] = cpu;
    if (count < 5) {
        std::fprintf(stderr, "tiered test requires five allowed CPUs\n");
        return 77;
    }
    allowed = mask({cpus[0], cpus[1], cpus[2], cpus[3], cpus[4]});
    assert(!sched_setaffinity(0, sizeof(allowed), &allowed));
    replay = mask({cpus[4]});
    recorder = mask({cpus[3]});
    remaining = mask({cpus[1], cpus[2]});
    const auto completion_allowed = mask({cpus[1], cpus[2], cpus[3], cpus[4]});
    const unsigned capacity[] = {200, 838, 838, 838, 1024};
    for (int i = 0; i < 5; ++i) {
        auto path = std::filesystem::path(argv[1]) / ("cpu" + std::to_string(cpus[i]));
        std::filesystem::create_directories(path);
        std::ofstream out(path / "cpu_capacity");
        assert(out << capacity[i] << '\n');
    }
    setenv("S2A_TEST_CPU_ROOT", argv[1], 1);
    unsetenv("S2A_TEST_DEBUG_GATE");
    unsetenv("S2A_DEBUG_CAPACITY_AFFINITY");

    // Property rollback reproduces split's single-maximum fallback on this topology.
    tiered_flag = 0;
    s2a_capacity_nvk_record_enter();
    s2a_capacity_enter(S2A_CAPACITY_REPLAY);
    auto actual = read_mask(0);
    assert(CPU_EQUAL(&actual, &allowed));
    s2a_capacity_leave(S2A_CAPACITY_REPLAY);
    s2a_capacity_test_reset_mode();
    tiered_flag = 1;

    // Start the same entry points used by the native roles.
    Worker workers[] = {
        {s2a_capacity_nvk_record_enter},
        {s2a_capacity_guest_enter},
        {[] { s2a_capacity_enter(S2A_CAPACITY_BACKGROUND); }},
        {omit_audio ? nullptr : s2a_capacity_audio_enter},
        {completion_startup},
    };
    pthread_t threads[5];
    assert(!pthread_barrier_init(&active, nullptr, 6));
    assert(!pthread_create(&threads[0], nullptr, worker_main, &workers[0]));
    pthread_attr_t completion_attr;
    assert(!pthread_attr_init(&completion_attr));
    assert(!pthread_attr_setaffinity_np(&completion_attr, sizeof(completion_allowed), &completion_allowed));
    assert(!pthread_create(&threads[4], &completion_attr, worker_main, &workers[4]));
    assert(!pthread_attr_destroy(&completion_attr));
    s2a_capacity_enter(S2A_CAPACITY_REPLAY);
    assert(sched_getcpu() == cpus[4]);
    pthread_t child;
    assert(!pthread_create(&child, nullptr, wide_child, nullptr));
    assert(!pthread_join(child, nullptr));
    for (int i = 1; i < 4; ++i)
        assert(!pthread_create(&threads[i], nullptr, worker_main, &workers[i]));
    rendezvous();
    auto check_tiered = [&] {
        auto actual = read_mask(0);
        assert(CPU_EQUAL(&actual, &replay));
        actual = read_mask(workers[0].tid);
        assert(CPU_EQUAL(&actual, &recorder));
        for (int i = 1; i < 4; ++i) {
            actual = read_mask(workers[i].tid);
            assert(!CPU_ISSET(cpus[4], &actual) && !CPU_ISSET(cpus[3], &actual));
            assert(CPU_EQUAL(&actual, &remaining));
        }
        actual = read_mask(workers[4].tid);
        assert(CPU_EQUAL(&actual, &completion_allowed));
    };
    check_tiered();
    s2a_capacity_foreground(0);
    actual = read_mask(0);
    assert(CPU_EQUAL(&actual, &allowed));
    for (int i = 0; i < 4; ++i) {
        actual = read_mask(workers[i].tid);
        assert(CPU_EQUAL(&actual, &allowed));
    }
    actual = read_mask(workers[4].tid);
    assert(CPU_EQUAL(&actual, &completion_allowed));
    s2a_capacity_foreground(1);
    check_tiered();
    s2a_capacity_leave(S2A_CAPACITY_REPLAY);
    rendezvous();
    for (auto thread : threads) assert(!pthread_join(thread, nullptr));
    assert(!pthread_barrier_destroy(&active));

    assert(!sched_setaffinity(0, sizeof(original), &original));
    std::puts("PASS tiered native: replay and recorder own separate CPUs; completion retains its inherited mask; guest, background and audio exclude both main CPUs");
    return 0;
}
