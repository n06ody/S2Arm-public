// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <functional>
#include <vector>
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
    if (!mode || cpus.empty() || role < 0 || role > 1) return {};
    auto sorted = cpus;
    for (const auto &cpu : sorted) if (!cpu.capacity) return {};
    std::sort(sorted.begin(), sorted.end(), [](const CpuCapacity &a, const CpuCapacity &b) {
        return a.capacity != b.capacity ? a.capacity > b.capacity : a.id > b.id;
    });
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
