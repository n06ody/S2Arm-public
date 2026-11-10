// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Keep the real record API declaration before renaming the component-only stub.
#include "direct/vk_record.h"
#include "nvn/nvn_vk_compute_phase.h"   // compute_phase 分段：真实记录链上断言各段有账/拒后为零
// Reuse compiler/cache/Vulkan boundary fixtures, not their direct-bridge main.
static void *mapped_storage;
static int freed;
#define main component_fixture_main
#define s2a_record_compute_trace component_record_compute_trace
#include "w2_compute_integration_test.c"
#undef main
#undef s2a_record_compute_trace
static int submitted, dispatched, barriers, commands;
static VkResult fence_result=VK_SUCCESS;
static struct s2a_vk_device record_device;
static struct s2a_record_result record_result;
static int memory_read(void *o,uint64_t a,void *b,size_t n) {
    // Pool descriptors are test-owned host structs; SSBO bytes use synthetic guest space.
    if(a>=0x1000 && a+n<=0x2000)return rd(o,a,b,n);
    memcpy(b,(void*)(uintptr_t)a,n);return 0;
}
static int failing_write(void *o,uint64_t a,const void*b,size_t n){if(mode==23){assert(freed==1);writes++;return -1;}return wr(o,a,b,n);}
static int packet(unsigned op,const void *p,size_t n) {
    unsigned char bytes[4096]={0};size_t z=(sizeof(op_hdr)+n+7)&~(size_t)7;
    op_hdr h={op,z};assert(z<=sizeof bytes);memcpy(bytes,&h,sizeof h);memcpy(bytes+sizeof h,p,n);
    struct s2a_record_memory mem={NULL,memory_read,failing_write};
    return s2a_record_exec_one(&record_device,bytes,z,&mem,&record_result);
}
// Synthetic host snapshot seam for the production compact BindProgram decoder.
// Entries remain live; keys/stage mapping are fixture inputs, not decoder outputs.
static p_bind_program program_snapshots[128];
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    if (!serial || serial >= 128 || program_snapshots[serial].snap.serial != serial) return NULL;
    const p_bind_program *p = &program_snapshots[serial];
    memcpy(keys, p->keys, sizeof p->keys); *ok = p->kok;
    memcpy(stage, p->stage_of, sizeof p->stage_of);
    return &p->snap;
}
static void bind_program(int stage,const uint8_t key[8],uint64_t serial) {
    p_bind_program p={0};p.prog=(void*)(uintptr_t)(0x1000+serial);p.stages=1<<stage;p.kok=8;
    memset(p.stage_of,255,sizeof p.stage_of);p.stage_of[3]=stage;p.snap.serial=serial;
    memcpy(p.keys[3],key,8); assert(serial < 128); program_snapshots[serial]=p;
    p_bind_program_id id={.prog=p.prog,.serial=serial,.stages=p.stages};
    _Static_assert(sizeof(p_bind_program_id)==24,"compact program payload");
    assert(!packet(OP_BIND_PROGRAM,&id,sizeof id));
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo *i,VkCommandBuffer *c) {
    (void)d;(void)i;*c=(VkCommandBuffer)(uintptr_t)(++commands);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i){(void)c;(void)i;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){(void)c;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer *c){(void)d;(void)p;(void)c;assert(n==1);freed++;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo *i,const VkAllocationCallbacks *a,VkFence *f){(void)d;(void)i;(void)a;*f=(VkFence)1;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks *a){(void)d;(void)f;(void)a;}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f){(void)q;(void)i;(void)f;assert(n==1&&dispatched==1&&barriers==1);submitted++;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout){(void)d;(void)f;(void)all;(void)timeout;assert(n==1);return fence_result;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline v){(void)c;assert(p==VK_PIPELINE_BIND_POINT_COMPUTE&&v);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t dc,const uint32_t *ds){(void)c;(void)l;(void)dc;(void)ds;assert(p==VK_PIPELINE_BIND_POINT_COMPUTE&&first==0&&n==2&&s[0]&&s[1]);}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){(void)c;assert(x==2&&y==3&&z==4);assert(reads==1&&mapped_storage);assert(((uint8_t*)mapped_storage)[0]==0x34);memset(mapped_storage,0x78,16);dispatched++;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *i){(void)c;(void)b;(void)i;assert(dispatched==1&&src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&dst==VK_PIPELINE_STAGE_HOST_BIT&&!dep&&nm==1&&!nb&&!ni&&m->srcAccessMask==VK_ACCESS_SHADER_WRITE_BIT&&m->dstAccessMask==VK_ACCESS_HOST_READ_BIT);barriers++;}
static void *metrics_worker(void *unused) {
    (void)unused;
    struct s2a_compute_diagnostic d={.session=12,.generation=23,.identity_valid=1,.source=S2A_COMPUTE_OFFLINE};
    for(int i=0;i<100;i++) {
        s2a_record_compute_note(&d,S2A_COMPUTE_COMPLETED,"");
        struct s2a_record_compute_stats snapshot;s2a_record_compute_snapshot(&snapshot);
        assert(snapshot.completed==snapshot.dispatches&&snapshot.sources[S2A_COMPUTE_OFFLINE]==snapshot.dispatches);
    }
    return NULL;
}
static void metrics_contract(void) {
    struct s2a_record_compute_stats s;
    assert(!s2a_record_compute_session_begin(12,23));
    assert(s2a_record_compute_session_begin(999,1)==-1);
    assert(!s2a_record_compute_trace(1));
    pthread_t threads[4];for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,metrics_worker,NULL));
    for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    s2a_record_compute_snapshot(&s);assert(s.dispatches==400&&s.completed==400&&s.trace_lines==64);
    assert(!s2a_record_compute_session_begin(12,23));s2a_record_reset_state();
    s2a_record_compute_snapshot(&s);assert(s.dispatches==400);
    struct s2a_compute_diagnostic d={.session=12,.generation=23,.source=S2A_COMPUTE_UNKNOWN};
    s2a_record_compute_note(&d,S2A_COMPUTE_BUILD_FAILED,"sensitive_shader_bytes /private/title/secret");
    s2a_record_compute_note(&d,S2A_COMPUTE_BUILD_FAILED,"compute miss: live compilation disabled");
    s2a_record_compute_snapshot(&s);
    assert(!s.reason_overflow&&s.reasons[0].count==1&&!strcmp(s.reasons[0].why,"unknown_failure"));
    assert(s.reasons[1].count==1&&!strcmp(s.reasons[1].why,"live_disabled")&&s.trace_lines==64);
    uint64_t n=s.dispatches;d.session=999;s2a_record_compute_note(&d,S2A_COMPUTE_COMPLETED,"");
    s2a_record_compute_snapshot(&s);assert(s.dispatches==n&&s.stale_events==1);
    assert(s2a_record_compute_session_end()==1);assert(s2a_record_compute_session_end()==0);
    s2a_record_compute_snapshot(&s);assert(!s.active&&s.dispatches==n);
    assert(s2a_record_compute_trace(1)==-1);
    assert(!s2a_record_compute_session_begin(13,24));s2a_record_compute_snapshot(&s);
    assert(!s.dispatches&&!s.trace_lines&&!s.trace&&!s.reason_overflow&&!s.stale_events);
    assert(!s2a_record_compute_trace(1));
    d.session=13;d.generation=24;
    const char *sensitive[]={"sensitive_shader_bytes /private/title/secret", "compute never submitted /private/title/secret", "shader{secret}\nasset-secret"};
    for(unsigned i=0;i<3;i++)s2a_record_compute_note(&d,S2A_COMPUTE_BUILD_FAILED,sensitive[i]);
    s2a_record_compute_snapshot(&s);
    assert(s.reasons[0].count==3&&!strcmp(s.reasons[0].why,"unknown_failure"));
    assert(s2a_record_compute_session_end()==1);
}
int main(int argc,char **argv) {
    assert(argc==4);int scenario=atoi(argv[1]);mode=scenario;good=argv[2];bad=argv[3];
    setenv("S2A_SHADER_REVISION_ROOT","/revision",1);setenv("S2A_DIRECT_CACHE_DIR","/private",1);
    setenv("S2A_COMPUTE_PHASE","1",1);   // 打开九段仪器：构建期一次缓存，须早于首笔 dispatch
    const char *profile="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
    pc=(struct s2a_direct_compile_context){.session=11,.generation=22,.direct_abi=1,.translator_closure="closure",.profile_hash="sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.profile_json=profile,.profile_bytes=strlen(profile),.offline_profile_available=1};
    uint8_t ctl[0x760]={0},code[16]={1,2,3,4},key[32],graphics_key[8]={0xdd};
    put_u32(ctl+0x20,0x760);put_u32(ctl+0x748,8);put_u32(ctl+0x74c,1);put_u32(ctl+0x750,1);
    s2a_sha256 hash;s2a_sha256_init(&hash);s2a_sha256_update(&hash,ctl,sizeof ctl);s2a_sha256_update(&hash,code,sizeof code);s2a_sha256_final(&hash,key);
    assert(!s2a_source_keep(key,5,code,sizeof code,ctl,sizeof ctl));
    record_device=(struct s2a_vk_device){.device=(VkDevice)1,.physical=(VkPhysicalDevice)1};
    record_device.memory.memoryTypeCount=1;record_device.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(!s2a_record_compute_session_begin(11,22));
    s2a_record_reset_state();bind_program(5,key,42);bind_program(4,graphics_key,99);
    p_bind_buf sb={.stage=5,.index=7,.addr=0x1100,.size=16};assert(!packet(OP_BIND_SSBO,&sb,sizeof sb));
    sb.stage=4;sb.addr=0x1201;assert(!packet(OP_BIND_SSBO,&sb,sizeof sb));
    if(scenario==2){sb.stage=5;sb.index=15;sb.addr=0x1801;sb.size=4;assert(!packet(OP_BIND_SSBO,&sb,sizeof sb));}
    dpool_t pool={0};p_set_pool pp={.pool=&pool};pool.n=7;assert(!packet(OP_SET_TEX_POOL,&pp,sizeof pp));pool.n=9;assert(!packet(OP_SET_SAMP_POOL,&pp,sizeof pp));
    memset(guest,0x34,sizeof guest);
    if(scenario==20)stopped=1;
    if(scenario==21)s2a_record_set_submit_fault(2);
    if(scenario==22)s2a_record_set_submit_fault(1);
    char why[128];assert(!s2a_compile_worker_start(compile_fixture,NULL,why,sizeof why));
    assert(!pthread_mutex_lock(&execution_lock));p_dispatch dispatch={.x=2,.y=3,.z=4};
    int rc=packet(OP_DISPATCH,&dispatch,sizeof dispatch);assert(!pthread_mutex_unlock(&execution_lock));
    struct s2a_compile_worker_stats stats;s2a_compile_worker_stats(&stats);s2a_compile_worker_stop();
    if(scenario==11||scenario==12||scenario==20) {
        assert(rc!=0&&!pipeline&&!submitted&&!dispatched&&!live);
        if(scenario==11)assert(pauses==1&&!resumes&&!worker&&!stats.runs&&!stats.compiles);
        if(scenario==12)assert(pauses==1&&resumes==1&&worker==1&&strstr(record_result.why,"stopped"));
        if(scenario==20)assert(!acquired&&!offline&&!disk&&!worker);
    } else {
        assert(pipeline==1&&offline==1&&disk==1&&worker==1&&stats.runs==1&&stats.compiles==1);
        assert(acquired==1&&released==1&&pauses==1&&resumes==1);
        assert(dispatched==1&&barriers==1);
        if(scenario==21){assert(rc==S2A_RECORD_GPU&&!submitted&&!writes&&!live&&freed==1&&guest[256]==0x34);}
        else if(scenario==22){
            assert(rc==S2A_RECORD_GPU&&submitted==1&&live>0&&!writes&&!freed);
            uint32_t left=99;fence_result=VK_TIMEOUT;
            assert(s2a_record_release_retained(&record_device,1000000000,&left)==0&&left==1&&live>0&&!writes&&!freed);
            fence_result=VK_ERROR_DEVICE_LOST;
            assert(s2a_record_release_retained(&record_device,1000000000,&left)==0&&left==1&&live>0&&!writes&&!freed);
            fence_result=VK_SUCCESS;
            assert(s2a_record_release_retained(&record_device,1000000000,&left)==1&&left==0&&!live&&writes==1&&freed==1&&guest[256]==0x78);
            assert(s2a_record_release_retained(&record_device,1000000000,&left)==0&&left==0&&writes==1&&freed==1);
        } else if(scenario==23){assert(rc==S2A_RECORD_GPU&&submitted==1&&!live&&writes==1&&freed==1&&guest[256]==0x34);assert(strstr(record_result.why,"copyback failed"));} else assert(rc==0&&submitted==1&&!live&&writes==1&&freed==1&&guest[256]==0x78);
    }
    {
        // compute_phase 分段断言（真实记录链，宿主 vk 替身边界——不主张设备行为）：
        // 判「段是否走到」用 hits——微段在粗粒度钟下可合法记 0ns。
        // 0/2 成功场景九段全走过；11/12 在 lookup 内拒（record/submit 未走），20 在进 bridge 前拒
        // （九段全未走），21/22/23 走完 build+record+submit。
        uint64_t pseg[NVK_PHASE_SEGS], phits[NVK_PHASE_SEGS], psamp, plines;
        nvk_compute_phase_stats(pseg, phits, &psamp, &plines);
        if (scenario == 0 || scenario == 2) {
            for (int s = 0; s < NVK_PHASE_SEGS; s++) assert(phits[s]);
        } else if (scenario == 20) {
            for (int s = 0; s < NVK_PHASE_SEGS; s++) assert(!phits[s]);
        } else if (scenario == 11 || scenario == 12) {
            assert(phits[NVK_PHASE_PREPARE] && phits[NVK_PHASE_LOOKUP] && !phits[NVK_PHASE_PARSE]);
            assert(!phits[NVK_PHASE_RECORD] && !phits[NVK_PHASE_SUBMIT_WAIT]);
        } else {
            assert(phits[NVK_PHASE_PARSE] && phits[NVK_PHASE_RECORD] && phits[NVK_PHASE_SUBMIT_WAIT]);
            if (scenario == 21) assert(phits[NVK_PHASE_DESTROY] && !phits[NVK_PHASE_WRITEBACK]); // finish(-1)：只毁不写
            else assert(phits[NVK_PHASE_WRITEBACK] && phits[NVK_PHASE_DESTROY]);               // 22 靠 release_retained 补的 finish(0)，23 回写失败仍记时
        }
        // 宿主无 shim 挂点：先 begin 再 wall，无论本笔是否 armed 都确定结算 +1；
        // 再打满 64 样本验 emit 窗口（log 替身 strong 提供）。
        nvk_compute_phase_begin(); nvk_compute_phase_wall(999999);
        for (int i = 0; i < 64; i++) {
            nvk_compute_phase_begin(); nvk_compute_phase_add(NVK_PHASE_RECORD, 100); nvk_compute_phase_wall(200);
        }
        uint64_t seg2[NVK_PHASE_SEGS], hits2[NVK_PHASE_SEGS], samp2, lines2;
        nvk_compute_phase_stats(seg2, hits2, &samp2, &lines2);
        int was_armed = 0;
        for (int s = 0; s < NVK_PHASE_SEGS; s++) was_armed |= phits[s] != 0;
        assert(samp2 == psamp + 65 + (was_armed ? 1 : 0) && lines2 == plines + 1);
    }
    struct s2a_record_compute_stats csstats;
    s2a_record_compute_snapshot(&csstats);
    assert(csstats.dispatches==1);
    if(scenario==0||scenario==2)assert(csstats.completed==1&&csstats.submitted==1&&csstats.sources[S2A_COMPUTE_LIVE]==1);
    if(scenario==21)assert(csstats.unsubmitted==1&&!csstats.submitted&&csstats.sources[S2A_COMPUTE_LIVE]==1);
    if(scenario==23)assert(csstats.copyback_failed==1&&csstats.submitted==1&&!csstats.completed&&!csstats.unknown_completion);
    if(scenario==22)assert(csstats.unknown_completion==1&&csstats.submitted==1&&!csstats.completed);
    if(scenario==11||scenario==12||scenario==20)assert(csstats.build_failed==1&&!csstats.submitted);
    s2a_record_reset_state();s2a_record_compute_snapshot(&csstats);assert(csstats.dispatches==1);
    assert(s2a_record_compute_session_end()==1);assert(s2a_record_compute_session_end()==0);
    assert(!s2a_record_compute_session_begin(12,23));s2a_record_compute_snapshot(&csstats);assert(!csstats.dispatches);
    assert(guest[255]==0x34&&guest[272]==0x34);
    assert(offline<=1&&disk<=1&&worker<=1&&acquired<=1&&released<=1);
    s2a_source_clear();uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(!held);
    metrics_contract();
    printf("record-default-chain scenario=%d PASS submitted=%d freed=%d writes=%d lookup=%d/%d/%d pin=%d/%d\n",scenario,submitted,freed,writes,offline,disk,worker,acquired,released);return 0;
}
