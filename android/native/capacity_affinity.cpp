// SPDX-License-Identifier: Apache-2.0
// Reversible host placement; never changes guest virtual-core policy.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdlib>
#include "capacity_affinity.h"
#include "capacity_policy.h"
#include "capacity_snapshot.h"
#if defined(S2A_CAPACITY_TEST)
static bool s2a_debug_entries_open() { const char *v=std::getenv("S2A_TEST_DEBUG_GATE"); return v && v[0]=='1'; }
#else
#include "debug_gate.h"
#endif
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <cstdarg>
#include <map>
#include <pthread.h>
#include <string>
#include <vector>

namespace {
std::mutex mu;
struct Slot { pid_t tid=0; cpu_set_t *original=nullptr, *preferred=nullptr, *scratch=nullptr; size_t bytes=0; bool applied=false; } slots[2];
// Only threads observed through our linker wrapper are known; other coverage is unknown.
std::map<pid_t,std::string> wrapped_original;
bool foreground=true;
bool stopping=false;
bool topology_failed=false;
// Only registered role threads can inherit a narrowed mask. Avoid touching
// global placement state on unrelated or pre-initialization pthread calls.
thread_local bool role_thread=false;
long long boot_us() { timespec t{}; return clock_gettime(CLOCK_BOOTTIME, &t)==0 ? static_cast<long long>(t.tv_sec)*1000000LL+t.tv_nsec/1000 : -1; }
std::string mask_string(const cpu_set_t *mask, size_t bytes) {
    std::string out;
    for(size_t i=0;i<bytes*8;++i) if(CPU_ISSET_S(i,bytes,mask)) {
        if(!out.empty()) out+=",";
        out+=std::to_string(i);
    }
    return out.empty()?"-":out;
}
// Production defaults to split. Debug mode may explicitly disable or select another strategy.
const char *read_capacity_mode() {
    if (s2a_debug_entries_open()) {
        const char *v = std::getenv("S2A_DEBUG_CAPACITY_AFFINITY");
        if (v) {
            if (!std::strcmp(v,"0")) return "0";
            if (!std::strcmp(v,"1")) return "1";
            if (!std::strcmp(v,"split")) return "split";
            if (!std::strcmp(v,"split2")) return "split2";
            if (!std::strcmp(v,"top2grp")) return "top2grp";
        }
    }
    return "split";
}
// pthread_create can be called before debug_env is installed. Observe without
// freezing until the first guest role registers after debug environment seal.
std::atomic<const char *> frozen_mode{nullptr};
const char *capacity_mode() {
    const char *mode = frozen_mode.load(std::memory_order_acquire);
    return mode ? mode : read_capacity_mode();
}
void freeze_mode() {
    const char *expected = nullptr;
    frozen_mode.compare_exchange_strong(expected, read_capacity_mode(), std::memory_order_acq_rel);
}
#if defined(S2A_CAPACITY_TEST)
extern "C" void s2a_capacity_test_reset_mode() { frozen_mode.store(nullptr, std::memory_order_release); topology_failed=false; }
#endif
bool enabled() { return std::strcmp(capacity_mode(), "0") != 0; }
void debug_log(const char *fmt, ...) {
    if (!s2a_debug_entries_open()) return;
    va_list args; va_start(args, fmt); std::vfprintf(stderr, fmt, args); va_end(args);
}
void clear(Slot &s) { if(s.original) CPU_FREE(s.original); if(s.preferred) CPU_FREE(s.preferred); if(s.scratch) CPU_FREE(s.scratch); s.original=s.preferred=s.scratch=nullptr; s.bytes=0; s.applied=false; }
void restore(Slot &s, int role, bool report) {
    if(!s.applied) { clear(s); return; }
    cpu_set_t *actual=s.scratch;
    int rc=sched_getaffinity(s.tid,s.bytes,actual), er=rc?errno:0;
    if(!rc && CPU_EQUAL_S(s.bytes,actual,s.preferred)) {
        rc=sched_setaffinity(s.tid,s.bytes,s.original); er=rc?errno:0;
        if(!rc) { rc=sched_getaffinity(s.tid,s.bytes,actual); er=rc?errno:0; if(!rc && !CPU_EQUAL_S(s.bytes,actual,s.original)) er=EAGAIN; }
    } else if(!rc) er=EAGAIN; // A different actor changed it: do not overwrite.
    if (er) std::fprintf(stderr,"[s2a-capacity] role=%d tid=%d restore=unverified errno=%d\n",role,s.tid,er);
    else if(report) debug_log("[s2a-capacity] role=%d tid=%d boot_us=%lld restore=verified errno=0\n",role,s.tid,boot_us());
    clear(s);
}
// Fail closed to mode0 for both roles: a partial placement is not split.
void fallback_to_mode0(int role, pid_t tid, const char *reason, int error) {
    std::fprintf(stderr,"[s2a-capacity] fallback=mode0 role=%d tid=%d reason=%s errno=%d\n",role,tid,reason,error);
    topology_failed=true;
    for (int i=0; i<2; ++i) restore(slots[i],i,true);
}
void apply(Slot &s,int role) {
  try {
    if(!s.tid || !foreground || stopping || topology_failed || !enabled() || s.applied) return;
    long cpus=sysconf(_SC_NPROCESSORS_CONF);
    if(cpus<=0 || cpus>65536) { fallback_to_mode0(role,s.tid,"cpu-count",0); return; }
    size_t n=static_cast<size_t>(cpus); s.bytes=CPU_ALLOC_SIZE(n); s.original=CPU_ALLOC(n); s.preferred=CPU_ALLOC(n); s.scratch=CPU_ALLOC(n);
    if(!s.original || !s.preferred || !s.scratch) { fallback_to_mode0(role,s.tid,"allocation",ENOMEM); return; }
    CPU_ZERO_S(s.bytes,s.original); CPU_ZERO_S(s.bytes,s.preferred);
    if(sched_getaffinity(s.tid,s.bytes,s.original)) { int er=errno; fallback_to_mode0(role,s.tid,"get-affinity",er); return; }
    std::vector<s2a::CpuCapacity> cap; unsigned low=~0u,high=0;
    for(size_t i=0;i<n;++i) if(CPU_ISSET_S(i,s.bytes,s.original)) {
        char path[512];
#if defined(S2A_CAPACITY_TEST)
        const char *root=std::getenv("S2A_TEST_CPU_ROOT");
        std::snprintf(path,sizeof(path),"%s/cpu%zu/cpu_capacity",root?root:"",i);
#else
        std::snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%zu/cpu_capacity",i);
#endif
        FILE *f=std::fopen(path,"r"); unsigned v=0; int ok=f && std::fscanf(f,"%u",&v)==1 && v; if(f) std::fclose(f);
        if(!ok) { fallback_to_mode0(role,s.tid,"capacity-unavailable",0); return; }
        cap.push_back({static_cast<unsigned>(i),v}); if(v<low)low=v; if(v>high)high=v;
    }
    const char *mode=capacity_mode();
    const auto selected=s2a::capacity_cpus(cap,mode,role);
    if(selected.empty()) {
        const char *reason = cap.size()<2 ? "insufficient-cpus" : "homogeneous";
        if ((!std::strcmp(mode,"split") || !std::strcmp(mode,"split2")) &&
            s2a::highest_capacity_cpus(cap).size() == 1) reason = "highest-group-single";
        fallback_to_mode0(role,s.tid,reason,0); return;
    }
    for(unsigned i:selected) CPU_SET_S(i,s.bytes,s.preferred);
    Slot &other=slots[1-role];
    if ((!std::strcmp(mode,"split") || !std::strcmp(mode,"split2")) && other.applied && other.bytes==s.bytes && CPU_EQUAL_S(s.bytes,other.preferred,s.preferred)) {
        fallback_to_mode0(role,s.tid,"split-collision",0); return;
    }
    if(sched_setaffinity(s.tid,s.bytes,s.preferred)) { int er=errno; fallback_to_mode0(role,s.tid,"set-affinity",er); return; }
    s.applied=true;
    cpu_set_t *actual=s.scratch;
    int er=0;
    if(actual && sched_getaffinity(s.tid,s.bytes,actual)) er=errno;
    if(actual && !er && !CPU_EQUAL_S(s.bytes,actual,s.preferred)) er=EAGAIN;
    if (er) { fallback_to_mode0(role,s.tid,"apply-readback",er); return; }
    if (s2a_debug_entries_open()) debug_log("[s2a-capacity] role=%d tid=%d boot_us=%lld mode=%s allowed=%s requested=%s actual=%s capacity=%u..%u apply=verified errno=0\n",role,s.tid,boot_us(),mode,
        mask_string(s.original,s.bytes).c_str(),mask_string(s.preferred,s.bytes).c_str(),
        mask_string(actual,s.bytes).c_str(),low,high);
  } catch (...) {
    // Diagnostic allocation must not terminate the game; undo any placement we made.
    fallback_to_mode0(role,s.tid,"allocation",ENOMEM);
  }
}
}
extern "C" void s2a_capacity_enter(int role) { if(role<0||role>1)return; freeze_mode(); if(!enabled())return; std::lock_guard<std::mutex> lock(mu); Slot &s=slots[role]; if(s.tid) return; s.tid=static_cast<pid_t>(syscall(SYS_gettid)); role_thread=true; apply(s,role); }
extern "C" void s2a_capacity_leave(int role) { if(role<0||role>1||!enabled())return; std::lock_guard<std::mutex> lock(mu); Slot &s=slots[role]; if(s.tid!=syscall(SYS_gettid))return; restore(s,role,true); s.tid=0; role_thread=false; }
extern "C" void s2a_capacity_foreground(int fg) { if(!enabled())return; std::lock_guard<std::mutex> lock(mu); if(stopping)return; foreground=fg!=0; for(int i=0;i<2;++i) if(foreground)apply(slots[i],i);else restore(slots[i],i,true); }
// 诊断只低频观察线程掩码；枚举不是原子快照，退出/权限/长度上限均明确报不完整。
static void sample_thread_masks() {
    if (!s2a_debug_entries_open()) return;
    const char *v=std::getenv("S2A_DEBUG_CAPACITY_AFFINITY");
    if (!v || (std::strcmp(v,"0") && std::strcmp(v,"1") && std::strcmp(v,"split") && std::strcmp(v,"split2") && std::strcmp(v,"top2grp"))) return;
    static long long last=0;
    long long begin=boot_us(); if(begin<0 || (last && begin-last<15000000LL)) return; last=begin;
    try {
        auto snap=s2a::read_affinity_snapshot("/proc/self/task");
        debug_log("[s2a-capacity-threads] begin=%lld seen=%u records=%zu errors=%u truncated=%d\n",begin,snap.seen,snap.records.size(),snap.errors,snap.truncated);
        size_t groups=0;
        std::string group;
        for(const auto &record:snap.records) {
            std::string original="unknown", coverage="unknown";
            {
                std::lock_guard<std::mutex> lock(mu);
                for(int i=0;i<2;++i) if(slots[i].applied && slots[i].tid==static_cast<pid_t>(record.tid)) {
                    original=mask_string(slots[i].original,slots[i].bytes); coverage=i==0?"role0":"role1";
                }
                auto it=wrapped_original.find(static_cast<pid_t>(record.tid));
                if(it!=wrapped_original.end()) { original=it->second; coverage="wrapped"; }
            }
            std::string item=std::to_string(record.tid)+":"+record.name+":"+record.mask+":"+original+":"+coverage+";";
            if (!group.empty() && group.size()+item.size()>1024) {
                debug_log("[s2a-capacity-threads] sample=%lld group=%zu entries=%s\n",begin,groups++,group.c_str()); group.clear();
            }
            group+=item;
        }
        if (!group.empty()) debug_log("[s2a-capacity-threads] sample=%lld group=%zu entries=%s\n",begin,groups++,group.c_str());
        debug_log("[s2a-capacity-threads] end=%lld sample=%lld records=%zu groups=%zu\n",boot_us(),begin,snap.records.size(),groups);
    } catch (...) { debug_log("[s2a-capacity-threads] sample=%lld error=allocation\n",begin); }
}
extern "C" void s2a_capacity_check(void) {
    sample_thread_masks();
    if(!enabled())return;
    std::lock_guard<std::mutex> lock(mu);
    for(int i=0;i<2;++i) {
        Slot &s=slots[i]; if(!s.applied)continue;
        int rc=sched_getaffinity(s.tid,s.bytes,s.scratch);
        if(rc || !CPU_EQUAL_S(s.bytes,s.scratch,s.preferred)) {
            debug_log("[s2a-capacity] role=%d tid=%d boot_us=%lld drift errno=%d\n",i,s.tid,boot_us(),rc?errno:EAGAIN); clear(s);
        } else {
            debug_log("[s2a-capacity] role=%d tid=%d boot_us=%lld check=verified\n",i,s.tid,boot_us());
        }
    }
}

extern "C" void s2a_capacity_stop(void) { if(!enabled())return; std::lock_guard<std::mutex> lock(mu); stopping=true; foreground=false; for(int i=0;i<2;++i) restore(slots[i],i,true); }

#if defined(S2A_CAPACITY_WRAP)
// Linker --wrap covers references in this shared object's own objects / static
// archives; calls made wholly inside separately loaded libraries are unknown.
struct ChildAffinity {
    void *(*start)(void *);
    void *arg;
    size_t bytes;
    cpu_set_t *original;
    cpu_set_t *preferred;
};
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static void forget_wrapped(void *tracked) {
    if (!tracked) return;
    std::lock_guard<std::mutex> lock(mu);
    wrapped_original.erase(static_cast<pid_t>(syscall(SYS_gettid)));
}
static void *child_entry(void *raw) {
    ChildAffinity *c=static_cast<ChildAffinity *>(raw);
    void *(*start)(void *)=c->start; void *arg=c->arg;
    pid_t tid=static_cast<pid_t>(syscall(SYS_gettid));
    cpu_set_t *actual=static_cast<cpu_set_t *>(std::malloc(c->bytes));
    int error=0; const char *result="skipped"; bool have_actual=false;
    bool tracked = false;
    const bool diagnostics = s2a_debug_entries_open();
    std::string inherited="unknown";
    if (!actual) error=ENOMEM;
    else if (sched_getaffinity(tid,c->bytes,actual)) error=errno;
    else {
        have_actual=true;
        if (diagnostics) { try { inherited=mask_string(actual,c->bytes); } catch (...) {} }
        if (CPU_EQUAL_S(c->bytes,actual,c->preferred)) {
            if (sched_setaffinity(tid,c->bytes,c->original)) error=errno;
            else if (sched_getaffinity(tid,c->bytes,actual)) { error=errno; have_actual=false; }
            else if (!CPU_EQUAL_S(c->bytes,actual,c->original)) error=EAGAIN;
            else result="restored";
        } else if (CPU_EQUAL_S(c->bytes,actual,c->original)) result="already-original";
        else error=EAGAIN; // Another actor changed the inherited mask; do not overwrite.
    }
    if (diagnostics) try {
        std::string original=mask_string(c->original,c->bytes);
        std::string current=have_actual ? mask_string(actual,c->bytes) : "unknown";
        { std::lock_guard<std::mutex> lock(mu); wrapped_original[tid]=original; tracked=true; }
        debug_log("[s2a-capacity-child] tid=%d boot_us=%lld original=%s inherited=%s actual=%s result=%s errno=%d\n",tid,boot_us(),original.c_str(),inherited.c_str(),current.c_str(),result,error);
    } catch (...) { debug_log("[s2a-capacity-child] tid=%d error=allocation\n",tid); }
    std::free(actual); CPU_FREE(c->original); CPU_FREE(c->preferred); std::free(c);
    void *returned;
    pthread_cleanup_push(forget_wrapped,tracked ? reinterpret_cast<void *>(1) : nullptr);
    returned=start(arg);
    pthread_cleanup_pop(1);
    return returned;
}
extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg) {
    if (!role_thread || std::strcmp(capacity_mode(),"split2")) return __real_pthread_create(thread,attr,start,arg);
    ChildAffinity *child=nullptr; bool role_found=false;
    {
        std::lock_guard<std::mutex> lock(mu);
        pid_t tid=static_cast<pid_t>(syscall(SYS_gettid));
        for (auto &s:slots) if(s.applied && s.tid==tid) {
            role_found=true;
            child=static_cast<ChildAffinity *>(std::calloc(1,sizeof(*child)));
            if (!child) break;
            child->bytes=s.bytes; child->original=CPU_ALLOC(s.bytes*8); child->preferred=CPU_ALLOC(s.bytes*8);
            if (!child->original || !child->preferred) { if(child->original)CPU_FREE(child->original); if(child->preferred)CPU_FREE(child->preferred); std::free(child); child=nullptr; break; }
            std::memcpy(child->original,s.original,s.bytes); std::memcpy(child->preferred,s.preferred,s.bytes);
            child->start=start; child->arg=arg; break;
        }
    }
    if(!child) { if(role_found) debug_log("[s2a-capacity-child] error=capture-allocation\n"); return __real_pthread_create(thread,attr,start,arg); }
    int rc=__real_pthread_create(thread,attr,child_entry,child);
    if(rc) { CPU_FREE(child->original); CPU_FREE(child->preferred); std::free(child); }
    return rc;
}
#endif

#if defined(S2A_CAPACITY_WRAP)
namespace {
struct WideCall {
    int (*operation)(void *); void *argument; size_t bytes; pid_t caller;
    cpu_set_t *original; cpu_set_t *preferred;
    std::mutex lock; std::condition_variable done_cv;
    bool done=false; int status=0; int result=0;
};
void *wide_call_worker(void *raw) {
    WideCall *job=static_cast<WideCall *>(raw);
    pid_t tid=static_cast<pid_t>(syscall(SYS_gettid));
    cpu_set_t *actual=static_cast<cpu_set_t *>(std::malloc(job->bytes));
    int error=0;
    if (!actual) error=ENOMEM;
    else if (sched_getaffinity(tid,job->bytes,actual)) error=errno;
    else if (CPU_EQUAL_S(job->bytes,actual,job->preferred)) {
        if (sched_setaffinity(tid,job->bytes,job->original) || sched_getaffinity(tid,job->bytes,actual)) error=errno;
        else if (!CPU_EQUAL_S(job->bytes,actual,job->original)) error=EAGAIN;
    } else if (!CPU_EQUAL_S(job->bytes,actual,job->original)) error=EAGAIN;
    if (!error) {
        try {
            std::string original=mask_string(job->original,job->bytes);
            debug_log("[s2a-capacity-call] caller=%d tid=%d boot_us=%lld original=%s actual=%s ready=verified\n",job->caller,tid,boot_us(),original.c_str(),original.c_str());
        } catch (...) { debug_log("[s2a-capacity-call] caller=%d tid=%d ready=verified mask=allocation-error\n",job->caller,tid); }
        job->result=job->operation(job->argument);
    } else debug_log("[s2a-capacity-call] caller=%d tid=%d ready=failed errno=%d\n",job->caller,tid,error);
    std::free(actual);
    { std::lock_guard<std::mutex> lock(job->lock); job->status=error; job->done=true; job->done_cv.notify_one(); }
    return nullptr;
}
}
extern "C" int s2a_capacity_call_wide(int (*operation)(void *), void *argument, int *result) {
    if (!role_thread || std::strcmp(capacity_mode(),"split2")) return 0;
    if (!operation || !result) return -EINVAL;
    WideCall job{}; job.operation=operation; job.argument=argument;
    pid_t caller=static_cast<pid_t>(syscall(SYS_gettid)); job.caller=caller;
    {
        std::lock_guard<std::mutex> lock(mu);
        for (auto &s:slots) if (s.applied && s.tid==caller) {
            job.bytes=s.bytes; job.original=CPU_ALLOC(s.bytes*8); job.preferred=CPU_ALLOC(s.bytes*8);
            if (!job.original || !job.preferred) { if(job.original)CPU_FREE(job.original); if(job.preferred)CPU_FREE(job.preferred); return -ENOMEM; }
            std::memcpy(job.original,s.original,s.bytes); std::memcpy(job.preferred,s.preferred,s.bytes); break;
        }
    }
    if (!job.original) { debug_log("[s2a-capacity-call] caller=%d boot_us=%lld skip=not-role\n",caller,boot_us()); return 0; }
    debug_log("[s2a-capacity-call] caller=%d boot_us=%lld action=helper\n",caller,boot_us());
    // The argument is stack-owned until the worker signals completion. Disable
    // pthread cancellation for this interval so it cannot outlive the caller.
    int error=0;
#if !defined(__ANDROID__)
    // Android bionic has no pthread cancellation API.
    int old_cancel=0; error=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old_cancel);
    if (error) { CPU_FREE(job.original); CPU_FREE(job.preferred); return -error; }
#endif
    pthread_t thread; error=__real_pthread_create(&thread,nullptr,wide_call_worker,&job);
    if (!error) {
        { std::unique_lock<std::mutex> lock(job.lock); job.done_cv.wait(lock,[&job]{ return job.done; }); }
        int join_error=pthread_join(thread,nullptr);
        if(join_error) { debug_log("[s2a-capacity-call] caller=%d join_errno=%d\n",caller,join_error); pthread_detach(thread); }
        error=job.status; if(!error) *result=job.result;
    }
    CPU_FREE(job.original); CPU_FREE(job.preferred);
#if !defined(__ANDROID__)
    pthread_setcancelstate(old_cancel,nullptr);
#endif
    return error ? -error : 1;
}
#else
extern "C" int s2a_capacity_call_wide(int (*)(void *), void *, int *) { return 0; }
#endif
