#define _GNU_SOURCE 1
#include "../../native/capacity_affinity.h"
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
static unsigned mask=7;
static int sets=0, gets=0, deny=0;
extern "C" int __wrap_sched_getaffinity(pid_t, size_t size, cpu_set_t *out) noexcept {
    ++gets; CPU_ZERO_S(size,out); for(int i=0;i<3;++i)if(mask&(1u<<i))CPU_SET_S(i,size,out); return 0;
}
extern "C" int __wrap_sched_setaffinity(pid_t, size_t size, const cpu_set_t *in) noexcept {
    ++sets; if(deny) {errno=EPERM;return -1;} mask=0;for(int i=0;i<3;++i)if(CPU_ISSET_S(i,size,in))mask|=1u<<i;return 0;
}
static void capacity(const char *root, int cpu, int value) {
 char dir[512],path[512];std::snprintf(dir,sizeof(dir),"%s/cpu%d",root,cpu);mkdir(dir,0700);
 std::snprintf(path,sizeof(path),"%s/cpu%d/cpu_capacity",root,cpu);FILE *f=std::fopen(path,"w");assert(f);std::fprintf(f,"%d\n",value);std::fclose(f);
}
int main() {
 char root[]="/tmp/s2a-capacity-XXXXXX";assert(mkdtemp(root));setenv("S2A_TEST_CPU_ROOT",root,1);
 capacity(root,0,512);capacity(root,1,1024);capacity(root,2,1024);
 // Production default is split; closed gate ignores a split2 override.
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split2",1);
 s2a_capacity_enter(0); assert(mask==4 && sets==1); s2a_capacity_leave(0); assert(mask==7 && sets==2);
 sets=gets=0; setenv("S2A_TEST_DEBUG_GATE","1",1); s2a_capacity_test_reset_mode();
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","0",1);
 s2a_capacity_enter(0); assert(sets==0 && gets==0);
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","1",1); s2a_capacity_test_reset_mode();
 s2a_capacity_enter(0);assert(mask==6 && sets==1);
 s2a_capacity_foreground(0);assert(mask==7 && sets==2);
 s2a_capacity_foreground(1);assert(mask==6 && sets==3);
 s2a_capacity_leave(0);assert(mask==7 && sets==4);
 // Unknown roles and denied calls must not affect other threads.
 s2a_capacity_enter(2);assert(sets==4);deny=1;s2a_capacity_enter(1);assert(mask==7 && sets==5);s2a_capacity_leave(1);deny=0; s2a_capacity_test_reset_mode();
 // An external mask change is detected and is never overwritten during pause.
 s2a_capacity_enter(1);assert(mask==6);mask=3;int before=sets;s2a_capacity_check();s2a_capacity_foreground(0);assert(mask==3 && sets==before);s2a_capacity_leave(1);
 // Missing capacity / equal capacities mean no placement.
 s2a_capacity_foreground(1);mask=7;capacity(root,1,512);capacity(root,2,512);before=sets;s2a_capacity_enter(0);assert(sets==before && mask==7);s2a_capacity_leave(0);s2a_capacity_test_reset_mode();
 char missing[512];std::snprintf(missing,sizeof(missing),"%s/cpu1/cpu_capacity",root);assert(unlink(missing)==0);
 before=sets;s2a_capacity_enter(0);assert(sets==before && mask==7);s2a_capacity_leave(0);s2a_capacity_test_reset_mode();
 // Only one allowed CPU is insufficient for split and must leave the mask unchanged.
 capacity(root,1,512); mask=1; before=sets;
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split",1);s2a_capacity_test_reset_mode();
 s2a_capacity_enter(0);assert(mask==1 && sets==before);s2a_capacity_leave(0);s2a_capacity_test_reset_mode();
 // A unique highest-capacity CPU leaves both roles on their original masks.
 capacity(root,0,256);capacity(root,1,512);capacity(root,2,1024);mask=7;
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split",1);s2a_capacity_test_reset_mode();
 before=sets;s2a_capacity_enter(0);assert(mask==7 && sets==before);
 s2a_capacity_leave(0);s2a_capacity_test_reset_mode();
 // Split, top-two groups, and stop prevent late foreground reapply.
 capacity(root,1,1024);
 // Second role sees only the first role's CPU: revert the first role too.
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split",1);s2a_capacity_test_reset_mode();
 s2a_capacity_enter(0); assert(mask==4); s2a_capacity_enter(1); assert(mask==7);
 before=sets; s2a_capacity_foreground(1); assert(sets==before && mask==7);
 s2a_capacity_leave(1);s2a_capacity_leave(0);s2a_capacity_test_reset_mode();
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split",1);s2a_capacity_test_reset_mode();s2a_capacity_enter(0);assert(mask==4);s2a_capacity_leave(0);assert(mask==7);
 s2a_capacity_enter(1);assert(mask==2);s2a_capacity_leave(1);assert(mask==7);
 capacity(root,1,512);
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","top2grp",1);s2a_capacity_test_reset_mode();s2a_capacity_enter(0);assert(mask==6);
 s2a_capacity_check();s2a_capacity_stop();assert(mask==7);before=sets;
 s2a_capacity_foreground(1);assert(mask==7 && sets==before);s2a_capacity_leave(0);
 return 0;
}
