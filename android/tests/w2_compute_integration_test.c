// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production pthread worker + bridge/REQUEST/source/identity/lookup/parser/pipeline.
// Only compiler callback/cache IO and Vulkan calls are fixtures; not a GPU test.
#define _POSIX_C_SOURCE 200809L
#include "nvn/nvn_vk_compute.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"
#include "direct/compile_worker.h"
#include "direct/sha256.h"
#include "nvn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "integration_vk_mock.h"
#include "nvn/nvn_vk_compute_phase.h"   // compute_phase 分段：真实链上断言各段有账/拒后为零
extern int nvn_vk_compute_build(struct s2a_vk_device *,const struct s2a_record_dispatch_state *,
    const struct s2a_record_memory *,struct s2a_record_dispatch_output *,struct s2a_record_result *,void *);
static int acquired, released;
int real_source_acquire(const uint8_t key[8], struct s2a_source_entry_view *out);
void real_source_release(uint64_t ref);
int s2a_source_acquire(const uint8_t key[8], struct s2a_source_entry_view *out) { acquired++; return real_source_acquire(key,out); }
void s2a_source_release(uint64_t ref) { released++; real_source_release(ref); }
static int mode;
static int trace_enabled;
int s2a_record_compute_trace(int enabled) { trace_enabled=enabled; return 0; }
// Android 日志在宿主是空实现：只证明失败原因会进 logcat，不改变语义。
int __android_log_print(int prio, const char *tag, const char *fmt, ...) { (void)prio; (void)tag; (void)fmt; return 0; }
#ifndef W2_EXPECT_DEBUG
#define W2_EXPECT_DEBUG 0
#endif
int __system_property_get(const char *key, char *value) {
    const char *v="";
    if (!strcmp(key,"debug.s2a.debug_env")) v=mode==21?"0":mode==23?"01":mode==24?"true":"1";
    else if (!strcmp(key,"debug.s2a.no_live_compile")) v=mode==25?"01":mode>=21&&mode<=24?"1":"";
    else if (!strcmp(key,"debug.s2a.compute_trace")) v=mode>=21?"1":"";
    else if (!strcmp(key,"debug.s2a.offline_root")&&mode==26) v="/override";
    else if (!strcmp(key,"debug.s2a.no_offline")&&mode==27) v="1";
    else if (!strcmp(key,"debug.s2a.no_disk_cache")&&mode==28) v="1";
    strcpy(value,v); return (int)strlen(v);
}
static int provider_calls, offline, disk, worker, pauses, resumes, stopped;
// 注册表/解析替身来自 integration_vk_mock.h（w2_compute_pipeline_test.c 的 Vulkan mock 段，
// integration py 会把那里的 fallback 换成真实事实/对象），这里不再重复定义。
static pthread_mutex_t execution_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *good, *bad;
static struct s2a_direct_compile_context pc;
static uint8_t expected_identity[32];
static int read_bytes(const char *path,uint8_t **b,size_t *n) {
    FILE *f=fopen(path,"rb"); assert(f); fseek(f,0,SEEK_END);*n=(size_t)ftell(f);rewind(f);
    *b=malloc(*n);assert(*b&&fread(*b,1,*n,f)==*n);fclose(f);return 0;
}
// The producer echoes the REQUEST's own top-level requestId under result.transport.
// Fixture carries a <RID> placeholder; substitute the live requestId exactly where
// a real compiler would, so accept()'s fresh requestId echo check is a real equality
// against the request that produced these bytes (not a hard-coded fixture string).
static uint8_t *echo_request_id(uint8_t *b,size_t *n,const char *json) {
    const char *mark=strstr(json,"\"requestId\":\"");assert(mark);mark+=13;
    const char *end=strchr(mark,'"');assert(end);
    size_t idlen=(size_t)(end-mark);
    char *tok=strstr((char*)b,"<RID>");assert(tok);
    size_t head=(size_t)(tok-(char*)b),tail=*n-head-5;
    uint8_t *out=malloc(*n-5+idlen);assert(out);
    memcpy(out,b,head);memcpy(out+head,mark,idlen);memcpy(out+head+idlen,tok+5,tail);
    *n=*n-5+idlen;free(b);return out;
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    provider_calls++;*out=pc;if(mode==9)return -1;if(mode==10&&provider_calls>1)out->generation++;return 0;
}
int s2a_vk_adapter_exec_pause(void) { pauses++; if(mode==11)return -1; return pthread_mutex_unlock(&execution_lock); }
void s2a_vk_adapter_exec_resume(void) { assert(!pthread_mutex_lock(&execution_lock)); resumes++;if(mode==12)stopped=1; }
int s2a_vk_adapter_stop_requested(void) { return stopped; }
int s2a_offline_library_fetch(const char *root,const struct s2a_offline_request *q,
    struct s2a_offline_program *out,char *why,size_t cap) {
    assert(!strcmp(root,(W2_EXPECT_DEBUG&&mode==26)?"/override":"/revision"));assert(q->profile_declared);offline++;
    memcpy(expected_identity,q->identity,32);
    if(mode==13||mode==14)return read_bytes(mode==14?bad:good,&out->bytes,&out->len);
    snprintf(why,cap,"missing offline");return 1;
}
int s2a_program_cache_load(const char *root,const uint8_t id[32],uint8_t **b,size_t *n,
    uint8_t sha[32],char *why,size_t cap) {
    (void)b;(void)n;(void)sha;assert(!strcmp(root,"/private"));disk++;
    if(offline)assert(!memcmp(expected_identity,id,32));snprintf(why,cap,"missing private");return -1;
}
int s2a_program_cache_store_async(const char *root,const uint8_t id[32],const uint8_t *b,size_t n) {
    (void)id;assert(!strcmp(root,"/private")&&b&&n);return 0;
}
static int compile_fixture(void *ctx,uint64_t session,uint64_t generation,const char *json,size_t n,
    uint8_t **bytes,size_t *len,char *why,size_t cap) {
    (void)ctx;(void)n;assert(session==11&&generation==22);worker++;
    // 观察到的纹理绑定必须出现在 REQUEST 里：空表只在真的没有绑定时出现。
    if (mode == 5) assert(strstr(json,"\"textures\":[{\"stage\":\"cs\",\"handle\":14,"));
    else assert(strstr(json,"\"textures\":[]"));
    int unaligned=mode==1||mode==2;
    assert(strstr(json,unaligned?"\"hasUnalignedStorageBuffer\":true":"\"hasUnalignedStorageBuffer\":false"));
    s2a_source_clear();uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(held>0);
    if(mode==8||mode==14){snprintf(why,cap,"MISSING_FACT texture query");return -1;}
    int rc=read_bytes(good,bytes,len);if(!rc)*bytes=echo_request_id(*bytes,len,json);return rc;
}
static void put_u32(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int main(int argc,char **argv) {
    assert(argc==4);mode=atoi(argv[1]);good=argv[2];bad=argv[3];
    setenv("S2A_SHADER_REVISION_ROOT",mode==16?"relative":"/revision",1);
    setenv("S2A_DIRECT_CACHE_DIR",mode==16?"":"/private",1);
    setenv("S2A_COMPUTE_PHASE","1",1);   // 打开九段仪器：构建期一次缓存，须早于首笔 build
    const char *hash="sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *profile="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
    pc=(struct s2a_direct_compile_context){.session=11,.generation=22,.direct_abi=1,
        .translator_closure="closure",.profile_hash=hash,.profile_json=profile,.profile_bytes=strlen(profile),.offline_profile_available=1};
    uint8_t ctl[0x760]={0},code[16]={1,2,3,4},digest[32];
    put_u32(ctl+0x20,0x760);put_u32(ctl+0x748,8);put_u32(ctl+0x74c,1);put_u32(ctl+0x750,1);
    s2a_sha256 h;s2a_sha256_init(&h);s2a_sha256_update(&h,ctl,sizeof ctl);s2a_sha256_update(&h,code,sizeof code);s2a_sha256_final(&h,digest);
    struct s2a_record_dispatch_state state={.stages=32,.tex_pool_seen=1,.samp_pool_seen=1,.tex_pool_n=7,.samp_pool_n=9};
    memcpy(state.keys[0],digest,8);assert(!s2a_source_keep(state.keys[0],5,code,sizeof code,ctl,sizeof ctl));
    state.ssbo_seen=1u<<7;state.ssbo[7].addr=0x1100;state.ssbo[7].size=16;
    if(mode==1)state.ssbo[7].addr++;
    if(mode==2){state.ssbo_seen|=1u<<15;state.ssbo[15].addr=0x2001;state.ssbo[15].size=4;}
    if(mode==3){state.ssbo[15].addr=0x2001;state.ssbo[15].size=4;}
    if(mode==4){state.ssbo_seen|=1u<<15;state.ssbo[15].addr=0x2001;state.ssbo[15].size=0;}
    if(mode==5)state.tex_present[3]=1;if(mode==6)state.image_present[4]=1;if(mode==7)state.samp_present[5]=1;
    if(mode==17){pc.profile_json="{}";pc.profile_bytes=2;}
    if(mode==18)s2a_source_clear();if(mode==19)state.tex_pool_seen=0;if(mode==20)stopped=1;
    struct s2a_vk_device device={.device=(VkDevice)1,.physical=(VkPhysicalDevice)1};
    struct s2a_record_memory memory={.read=rd,.write=wr};
    memset(guest,0x34,sizeof guest);state.groups[0]=state.groups[1]=state.groups[2]=1;
    profile_mismatch=mode==15;
    char start_why[128];assert(!s2a_compile_worker_start(compile_fixture,NULL,start_why,sizeof start_why));
    assert(!pthread_mutex_lock(&execution_lock));
    struct s2a_record_dispatch_output out={0};struct s2a_record_result result={.executed=123};
    int rc=nvn_vk_compute_build(&device,&state,&memory,&out,&result,NULL);
    assert(!pthread_mutex_unlock(&execution_lock));
    struct s2a_compile_worker_stats worker_stats;s2a_compile_worker_stats(&worker_stats);
    s2a_compile_worker_stop();
    int success=mode<=7||mode==13||mode==16||(mode>=21&&!(W2_EXPECT_DEBUG&&mode==22));
    if(success){
        assert(rc==0&&pipeline==1&&out.finish&&out.owner&&live>0);
        struct compute_owner *owner=out.owner;assert(owner->count==1&&reads==1&&writes==0);
        assert(owner->buffers[0].guest==0x1100);assert(owner->buffers[0].size==(mode==1?17:16));
        assert(((uint8_t*)owner->buffers[0].mapped)[0]==0x34);
        memset(owner->buffers[0].mapped,0x78,(size_t)owner->buffers[0].size);
        assert(!out.finish(&device,out.owner,-2)&&live>0&&writes==0);
        assert(!out.finish(&device,out.owner,0)&&live==0&&writes==1&&guest[256]==0x78);
    }
    else {assert(rc!=0&&result.code==rc&&result.why[0]&&result.opcode==OP_DISPATCH&&result.executed==123);assert(!out.owner&&!out.pipeline);}
    {
        // compute_phase 分段断言（真实链）：判「段是否走到」用 hits——微段在粗粒度钟下可合法记 0ns。
        // 成功模式七段全走过（record/submit 两段不在本夹具范围）；被拒模式在拒点之后一律未走——
        // 8/14 在 lookup 内拒（前后段各自对账），17-20 早于 prepare 拒，15 在 build 内 profile 校验拒（parse 已过）。
        uint64_t seg[NVK_PHASE_SEGS], hits[NVK_PHASE_SEGS], samples, lines;
        nvk_compute_phase_stats(seg, hits, &samples, &lines);
        if (success) {
            assert(hits[NVK_PHASE_PREPARE] && hits[NVK_PHASE_LOOKUP] && hits[NVK_PHASE_PARSE]);
            assert(hits[NVK_PHASE_ALLOC_COPY] && hits[NVK_PHASE_DESCRIPTOR]);
            assert(hits[NVK_PHASE_WRITEBACK] && hits[NVK_PHASE_DESTROY]);
        } else {
            assert(!hits[NVK_PHASE_ALLOC_COPY] && !hits[NVK_PHASE_DESCRIPTOR]
                   && !hits[NVK_PHASE_WRITEBACK] && !hits[NVK_PHASE_DESTROY]);
            if (mode==15) assert(hits[NVK_PHASE_PARSE]);
            else assert(!hits[NVK_PHASE_PARSE]);
            if (mode==17||mode==18||mode==19||mode==20)
                assert(!hits[NVK_PHASE_PREPARE] && !hits[NVK_PHASE_LOOKUP]);
            if (mode==8||mode==11||mode==12||mode==14)
                assert(hits[NVK_PHASE_PREPARE] && hits[NVK_PHASE_LOOKUP]);
        }
    }
    // 6/7：独立图像/独立采样器“存在”不是查询证明，不再整笔拒绝；真使用者由编译器反射与 pipeline
    // 的 descriptor 分支明确失败。5：真实纹理绑定必须走完请求→编译→管线（事实表非空）。
    if(mode==5)assert(offline==1&&worker==1&&pipeline==1);
    if(mode==8||mode==14)assert(worker==1&&pipeline==0&&strstr(result.why,"MISSING_FACT"));
    if(mode==11)assert(pauses==1&&resumes==0&&pipeline==0&&worker==0&&worker_stats.compiles==0&&worker_stats.runs==0);
    if(mode==12)assert(pauses==1&&resumes==1&&pipeline==0&&strstr(result.why,"stopped"));
    if(mode==13)assert(offline==1&&disk==0&&worker==0&&pauses==0);
    if(mode==16)assert(!offline&&!disk&&worker==1);
    if((mode>=17&&mode<=20)||mode==9||mode==10)assert(!worker&&!pipeline);
    if(mode>=21) {
        int gate=W2_EXPECT_DEBUG&&mode!=21&&mode!=23&&mode!=24;
        assert(trace_enabled==gate);
        if(gate&&mode==22) assert(!worker&&!pipeline&&strstr(result.why,"live compilation disabled"));
        else assert(worker==1&&pipeline==1);
        assert(offline==!(gate&&mode==27));
        assert(disk==!(gate&&mode==28));
    }
    assert(worker_stats.compiles==(uint64_t)worker);
    assert(live==0);
    assert(offline<=1&&disk<=1&&worker<=1&&acquired<=1&&released<=1);
    if(success||worker||mode==10||mode==15)assert(acquired==1&&released==1);
    s2a_source_clear();uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(held==0);
    printf("integrated-real-worker case%d PASS lookup=%d/%d/%d pipeline=%d pause/resume=%d/%d\n",mode,offline,disk,worker,pipeline,pauses,resumes);return 0;
}
