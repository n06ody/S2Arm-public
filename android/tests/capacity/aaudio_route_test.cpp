#define _GNU_SOURCE 1
#include "../../native/aaudio_sink.h"
#include "../../native/capacity_affinity.h"
#include <aaudio/AAudio.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void fake_expect(const cpu_set_t *);
extern "C" void fake_fail(int);
extern "C" int fake_count(void);
extern "C" void fake_disconnect(void);
int main() {
 cpu_set_t full;CPU_ZERO(&full);assert(!sched_getaffinity(0,sizeof(full),&full));
 if(CPU_COUNT(&full)<3){std::fprintf(stderr,"need three allowed CPUs for two maximum cores and one lower core\n");return 77;}
 long conf=sysconf(_SC_NPROCESSORS_CONF);assert(conf>0 && conf<=CPU_SETSIZE);
 char root[]="/tmp/s2a-audio-affinity-XXXXXX";assert(mkdtemp(root));int top=-1,second=-1;
 for(int i=0;i<conf;++i)if(CPU_ISSET(i,&full)){second=top;top=i;}
 for(int i=0;i<conf;++i)if(CPU_ISSET(i,&full)){
  char dir[512],path[512];std::snprintf(dir,sizeof(dir),"%s/cpu%d",root,i);assert(!mkdir(dir,0700));
  std::snprintf(path,sizeof(path),"%s/cpu%d/cpu_capacity",root,i);FILE *f=std::fopen(path,"w");assert(f);std::fprintf(f,"%d\n",i==top || i==second ? 1024:512);std::fclose(f);
 }
 setenv("S2A_TEST_CPU_ROOT",root,1);setenv("S2A_TEST_DEBUG_GATE","1",1);
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split2",1);s2a_capacity_test_reset_mode();s2a_capacity_enter(0);
 cpu_set_t narrow;assert(!sched_getaffinity(0,sizeof(narrow),&narrow));assert(CPU_COUNT(&narrow)==1);
 fake_expect(&full);s2a_aaudio *audio=nullptr;
 assert(s2a_aaudio_open(1,48000,2,1000,&audio)==AAUDIO_OK && audio && fake_count()==1);
 fake_disconnect();s2a_aaudio_service(audio);assert(fake_count()==2);
 cpu_set_t after;assert(!sched_getaffinity(0,sizeof(after),&after));assert(CPU_EQUAL(&narrow,&after));
 s2a_aaudio_close(audio);fake_fail(1);audio=nullptr;
 assert(s2a_aaudio_open(1,48000,2,1000,&audio)==AAUDIO_ERROR_INTERNAL && audio==nullptr && fake_count()==3);
 fake_fail(0);s2a_capacity_leave(0);
 // Legacy split still inherits the caller's mask, preserving its diagnostic arm.
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","split",1);s2a_capacity_test_reset_mode();s2a_capacity_enter(0);
 assert(!sched_getaffinity(0,sizeof(narrow),&narrow));fake_expect(&narrow);audio=nullptr;
 assert(s2a_aaudio_open(1,48000,2,1000,&audio)==AAUDIO_OK && audio && fake_count()==4);
 s2a_aaudio_close(audio);s2a_capacity_leave(0);
 // Off uses ordinary full calling context.
 setenv("S2A_DEBUG_CAPACITY_AFFINITY","0",1);fake_expect(&full);audio=nullptr;
 assert(s2a_aaudio_open(1,48000,2,1000,&audio)==AAUDIO_OK && audio && fake_count()==5);s2a_aaudio_close(audio);
 return 0;
}
