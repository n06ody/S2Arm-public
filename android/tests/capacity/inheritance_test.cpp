#define _GNU_SOURCE 1
#include "../../native/capacity_affinity.h"
#include <sched.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static cpu_set_t full, external_mask;
static void *inspect_external(void *) {
    cpu_set_t actual; CPU_ZERO(&actual); assert(!sched_getaffinity(0,sizeof(actual),&actual));
    return reinterpret_cast<void *>(static_cast<uintptr_t>(CPU_EQUAL(&actual,&external_mask)));
}
static int call_count=0;
static int wide_operation(void *) {
    ++call_count;
    cpu_set_t actual; CPU_ZERO(&actual); assert(!sched_getaffinity(0,sizeof(actual),&actual));
    assert(CPU_EQUAL(&actual,&full));
    pthread_t descendant; assert(!pthread_create(&descendant,nullptr,inspect_external,nullptr));
    void *result=nullptr; assert(!pthread_join(descendant,&result));
    // The same full mask must be inherited by a call that would be inside an
    // external library; inspect_external uses the independently set expectation.
    assert(result==reinterpret_cast<void *>(1));
    return 42;
}
static void *inspect(void *arg) {
    cpu_set_t actual; CPU_ZERO(&actual); assert(!sched_getaffinity(0,sizeof(actual),&actual));
    bool expect_full=arg!=nullptr;
    bool correct=expect_full ? CPU_EQUAL(&actual,&full) : CPU_COUNT(&actual)==1;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(correct));
}
int main() {
    CPU_ZERO(&full); assert(!sched_getaffinity(0,sizeof(full),&full));
    if(CPU_COUNT(&full)<3) { std::fprintf(stderr,"need three allowed CPUs for two maximum cores and one lower core\n"); return 77; }
    long conf=sysconf(_SC_NPROCESSORS_CONF); assert(conf>0 && conf<=CPU_SETSIZE);
    char root[]="/tmp/s2a-inheritance-XXXXXX"; assert(mkdtemp(root));
    int top=-1, second=-1; for(int i=0;i<conf;++i)if(CPU_ISSET(i,&full)){second=top;top=i;}
    for(int i=0;i<conf;++i) if(CPU_ISSET(i,&full)) {
        char dir[512],path[512]; std::snprintf(dir,sizeof(dir),"%s/cpu%d",root,i); assert(!mkdir(dir,0700));
        std::snprintf(path,sizeof(path),"%s/cpu%d/cpu_capacity",root,i); FILE *f=std::fopen(path,"w"); assert(f);
        std::fprintf(f,"%d\n",i==top || i==second ? 1024:512); std::fclose(f);
    }
    setenv("S2A_TEST_CPU_ROOT",root,1);
    // A native thread may be created before debug_env arrives: it must not freeze the mode.
    unsetenv("S2A_TEST_DEBUG_GATE"); unsetenv("S2A_DEBUG_CAPACITY_AFFINITY");
    s2a_capacity_test_reset_mode(); pthread_t early; assert(!pthread_create(&early,nullptr,inspect,reinterpret_cast<void *>(1)));
    void *early_result=nullptr; assert(!pthread_join(early,&early_result)); assert(early_result==reinterpret_cast<void *>(1));
    setenv("S2A_TEST_DEBUG_GATE","1",1); setenv("S2A_DEBUG_CAPACITY_AFFINITY","0",1);
    s2a_capacity_enter(0); cpu_set_t disabled; assert(!sched_getaffinity(0,sizeof(disabled),&disabled)); assert(CPU_EQUAL(&disabled,&full));
    s2a_capacity_leave(0);
    // Default production split retains the narrowed mask in children and never uses the wide helper.
    for(int role:{0,1}) {
        unsetenv("S2A_TEST_DEBUG_GATE"); unsetenv("S2A_DEBUG_CAPACITY_AFFINITY"); s2a_capacity_test_reset_mode();
        s2a_capacity_enter(role); cpu_set_t narrow; assert(!sched_getaffinity(0,sizeof(narrow),&narrow)); assert(CPU_COUNT(&narrow)==1);
        pthread_t child; assert(!pthread_create(&child,nullptr,inspect,nullptr));
        void *result=nullptr; assert(!pthread_join(child,&result)); assert(result==reinterpret_cast<void *>(1));
        int untouched=0; assert(s2a_capacity_call_wide(wide_operation,nullptr,&untouched)==0 && call_count==0);
        s2a_capacity_leave(role); cpu_set_t restored; assert(!sched_getaffinity(0,sizeof(restored),&restored)); assert(CPU_EQUAL(&restored,&full));
    }
    // Explicit split2 in a debug session remains stable if the debug property changes.
    for(int role:{0,1}) {
        setenv("S2A_DEBUG_CAPACITY_AFFINITY","split2",1); setenv("S2A_TEST_DEBUG_GATE","1",1); s2a_capacity_test_reset_mode();
        s2a_capacity_enter(role); cpu_set_t narrow; assert(!sched_getaffinity(0,sizeof(narrow),&narrow)); assert(CPU_COUNT(&narrow)==1);
        pthread_t child; assert(!pthread_create(&child,nullptr,inspect,reinterpret_cast<void *>(1)));
        void *result=nullptr; assert(!pthread_join(child,&result)); assert(result==reinterpret_cast<void *>(1));
        unsetenv("S2A_TEST_DEBUG_GATE"); setenv("S2A_DEBUG_CAPACITY_AFFINITY","0",1);
        s2a_capacity_leave(role); cpu_set_t restored; assert(!sched_getaffinity(0,sizeof(restored),&restored)); assert(CPU_EQUAL(&restored,&full));
    }
    setenv("S2A_TEST_DEBUG_GATE","1",1);
    for(const char *mode:{"split","split2"}) for(int role:{0,1}) {
        setenv("S2A_DEBUG_CAPACITY_AFFINITY",mode,1); s2a_capacity_test_reset_mode(); s2a_capacity_enter(role);
        cpu_set_t narrow; CPU_ZERO(&narrow); assert(!sched_getaffinity(0,sizeof(narrow),&narrow)); assert(CPU_COUNT(&narrow)==1);
        bool want_full=!std::strcmp(mode,"split2"); pthread_t child;
        assert(!pthread_create(&child,nullptr,inspect,want_full?reinterpret_cast<void *>(1):nullptr));
        void *result=nullptr; assert(!pthread_join(child,&result)); assert(result==reinterpret_cast<void *>(1));
        s2a_capacity_leave(role); cpu_set_t restored; assert(!sched_getaffinity(0,sizeof(restored),&restored)); assert(CPU_EQUAL(&restored,&full));
    }
    // Actual synchronous operation runs on a verified-wide helper; its own
    // children inherit the wide mask while the caller remains narrowed.
    external_mask=full;
    setenv("S2A_DEBUG_CAPACITY_AFFINITY","split2",1); s2a_capacity_test_reset_mode(); s2a_capacity_enter(0);
    cpu_set_t caller_before, caller_after; assert(!sched_getaffinity(0,sizeof(caller_before),&caller_before));
    int value=0; assert(s2a_capacity_call_wide(wide_operation,nullptr,&value)==1);
    assert(value==42 && call_count==1); assert(!sched_getaffinity(0,sizeof(caller_after),&caller_after));
    assert(CPU_EQUAL(&caller_before,&caller_after)); s2a_capacity_leave(0);
    // An unpinned caller or explicit off must not run the operation via helper.
    assert(s2a_capacity_call_wide(wide_operation,nullptr,&value)==0 && call_count==1);
    setenv("S2A_DEBUG_CAPACITY_AFFINITY","0",1); s2a_capacity_test_reset_mode();
    assert(s2a_capacity_call_wide(wide_operation,nullptr,&value)==0 && call_count==1);
    // If another actor changed the parent's mask, do not widen the child's
    // inherited mask or overwrite that actor's choice on parent leave.
    setenv("S2A_DEBUG_CAPACITY_AFFINITY","split2",1); s2a_capacity_test_reset_mode(); s2a_capacity_enter(0);
    cpu_set_t preferred; CPU_ZERO(&preferred); assert(!sched_getaffinity(0,sizeof(preferred),&preferred));
    CPU_ZERO(&external_mask);
    for(int i=0;i<conf;++i) if(CPU_ISSET(i,&full) && !CPU_ISSET(i,&preferred)) { CPU_SET(i,&external_mask); break; }
    assert(CPU_COUNT(&external_mask)==1 && !sched_setaffinity(0,sizeof(external_mask),&external_mask));
    assert(s2a_capacity_call_wide(wide_operation,nullptr,&value)<0 && call_count==1);
    pthread_t child; assert(!pthread_create(&child,nullptr,inspect_external,nullptr));
    void *result=nullptr; assert(!pthread_join(child,&result)); assert(result==reinterpret_cast<void *>(1));
    s2a_capacity_leave(0); cpu_set_t actual; assert(!sched_getaffinity(0,sizeof(actual),&actual));
    assert(CPU_EQUAL(&actual,&external_mask)); assert(!sched_setaffinity(0,sizeof(full),&full));
    return 0;
}
