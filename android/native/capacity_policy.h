// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <functional>
#include <vector>
#include "capacity_affinity.h"
namespace s2a {
struct CpuCapacity { unsigned id, capacity; };
// A missing reading or a homogeneous allowed set is not evidence of a faster core.
inline std::vector<unsigned> highest_capacity_cpus(const std::vector<CpuCapacity>& cpus) {
    if(cpus.empty()) return {};
    unsigned lo=cpus.front().capacity, hi=lo;
    for(const auto &cpu:cpus) { if(!cpu.capacity)return {}; lo=std::min(lo,cpu.capacity); hi=std::max(hi,cpu.capacity); }
    if(lo==hi)return {};
    std::vector<unsigned> out;
    for(const auto &cpu:cpus) if(cpu.capacity==hi)out.push_back(cpu.id);
    return out;
}
inline std::vector<unsigned> capacity_cpus(const std::vector<CpuCapacity>& cpus, const char *mode, int role) {
    if (!mode || cpus.empty() || role < S2A_CAPACITY_REPLAY || role > S2A_CAPACITY_AUDIO) return {};
    auto sorted = cpus;
    for (const auto &cpu : sorted) if (!cpu.capacity) return {};
    std::sort(sorted.begin(), sorted.end(), [](const CpuCapacity &a, const CpuCapacity &b) {
        return a.capacity != b.capacity ? a.capacity > b.capacity : a.id > b.id;
    });
    if (std::strcmp(mode, "tiered") == 0) {
        if (sorted.size() < 2 || sorted.front().capacity == sorted.back().capacity) return {};
        if (role == S2A_CAPACITY_REPLAY || role == S2A_CAPACITY_RECORD)
            return {sorted[static_cast<size_t>(role)].id};
        std::vector<unsigned> out;
        // 仅凭容量区分性能核：允许在峰值的 3/4 以上迁移，包含与超大核接近的中档核。
        const auto fast = [&](const CpuCapacity &cpu) { return 4ULL * cpu.capacity >= 3ULL * sorted.front().capacity; };
        for (size_t i = 2; i < sorted.size(); ++i)
            if (fast(sorted[i])) out.push_back(sorted[i].id);
        if (!out.empty()) return out;
        // 性能核不足时，游戏作业可共享性能核，后台和音频可用剩余许可核。
        if (role == S2A_CAPACITY_GUEST) {
            for (const auto &cpu : sorted) if (fast(cpu)) out.push_back(cpu.id);
        } else {
            for (size_t i = 2; i < sorted.size(); ++i) out.push_back(sorted[i].id);
        }
        return out;
    }
    if (role > S2A_CAPACITY_RECORD) return {};
    if (std::strcmp(mode, "1") == 0) return highest_capacity_cpus(cpus);
    if ((std::strcmp(mode, "split") == 0 || std::strcmp(mode, "split2") == 0)) {
        auto top = highest_capacity_cpus(cpus);
        if (top.size() < 2) return {};
        std::sort(top.begin(), top.end(), std::greater<unsigned>());
        return {top[static_cast<size_t>(role)]};
    }
    if (std::strcmp(mode, "top2grp") == 0) {
        unsigned first = sorted.front().capacity, second = 0;
        for (const auto &cpu : sorted) if (cpu.capacity < first) { second = cpu.capacity; break; }
        if (!second) return {};
        std::vector<unsigned> out;
        for (const auto &cpu : sorted) if (cpu.capacity >= second) out.push_back(cpu.id);
        return out;
    }
    return {};
}
}
