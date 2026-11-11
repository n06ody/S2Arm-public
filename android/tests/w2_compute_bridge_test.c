// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production REQUEST/source store/identity/lookup/parser; worker/Vulkan boundaries mocked.
#define _POSIX_C_SOURCE 200809L
#include "nvn/nvn_vk_compute.h"
#include "direct/offline_library.h"
#include "direct/program_cache.h"
#include "direct/compile_worker.h"
#include "direct/sha256.h"
#include "nvn.h"
#include "nvn/nvn_vk_texcache.h"
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// 宿主 logcat 替身：诊断/“红行”走 nvn_vk_compute_bridge.c 的弱缝 __android_log_print；
// 这里收集全部行供唯一请求集发布失败的负控断言（无桩时桥内静默，不改变结果判定）。
static char logall[65536]; static int logcalls;
int __android_log_print(int prio,const char*tag,const char*fmt,...) {
    (void)prio;(void)tag;size_t off=strlen(logall);va_list a;va_start(a,fmt);
    vsnprintf(logall+off,sizeof logall-off,fmt,a);va_end(a);logcalls++;return 0;
}
// 共享调试门宿主桩（compile_export.c 以 S2A_COMPILE_EXPORT_HOST_TEST 编译时引用）：
// mode 29 = 门关，其余门开；与生产 #968 定案一致——正式包缺省不导出。
static int mode;
static int diag_gate_mode(void) { return mode != 29; }
int s2a_direct_diag_debug_open(void) { return diag_gate_mode(); }
extern int nvn_vk_compute_build(struct s2a_vk_device *,const struct s2a_record_dispatch_state *,
    const struct s2a_record_memory *,struct s2a_record_dispatch_output *,struct s2a_record_result *,void *);
static int acquired, released;
int real_source_acquire(const uint8_t key[8], struct s2a_source_entry_view *out);
void real_source_release(uint64_t ref);
int s2a_source_acquire(const uint8_t key[8], struct s2a_source_entry_view *out) { acquired++; return real_source_acquire(key,out); }
void s2a_source_release(uint64_t ref) { released++; real_source_release(ref); }
static int provider_calls, offline, disk, worker, pipeline, pauses, resumes, stopped, finished, src_seen, cached_attempts;
// 宿主替身：纹理注册表事实（真实 NVN 格式/目标枚举），mode 21 = id 未注册，mode 22 = 目标未映射。
int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t cap) {
    if (id == 99 || mode == 21) { snprintf(why, cap, "texture id %d is not registered", id); return -1; }
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->nvn_format = NVN_FORMAT_RGBA8;
    out->nvn_target = mode == 22 ? 0xdead : NVN_TEXTURE_TARGET_2D;
    out->layers = 1; out->levels = 1; out->samples = 1;
    return 0;
}
// mode 24：注册表在取快照期间换代（第一次 7、第二次 8）→ 快照必须整笔失败，不混两份。
static int reggen_calls;
uint64_t s2a_vk_adapter_registry_generation(void) {
    if (mode == 24) return ++reggen_calls == 1 ? 7 : 8;
    return 7;
}
// 宿主替身：对象解析与保活解除。mode 25 返回一个与事实不一致的对象（格式不同）→ 快照必须拒绝。
// mode 24（取快照期间注册表换代）走生产的新复核分支：
//   nvn_vk_compute_texture.c 在换代后对每个条目用 s2a_vk_adapter_tex_by_id 重新解析，只有
//   “再解析到的对象 != 快照里 pin 住的”才失败。这里让 mode 24 返回另一个对象，触发准确拒绝。
// 注意：该 weak 缝在生产里被直接调用（未判空），缺失时会 SIGSEGV——已报 Lead，本夹具提供它。
static struct nvn_vk_tex mock_tex;
static struct nvn_vk_tex mock_tex_changed;
int s2a_vk_adapter_tex_by_id(int id, struct nvn_vk_tex **out, char *why, size_t cap) {
    (void)id;
    if (out) *out = mode == 24 ? &mock_tex_changed : &mock_tex;
    if (why && cap) snprintf(why, cap, mode == 24 ? "object replaced during snapshot" : "");
    return 0;
}
static int bind_calls, release_calls, mutated;
static uint64_t bind_handles[4];
static VkImageView captured_view;
static int captured_count;
int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *device, uint64_t handle,
    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out, char *why, size_t cap) {
    (void)device;
    if (bind_calls < 4) bind_handles[bind_calls] = handle;
    bind_calls++;
    memset(&mock_tex, 0, sizeof mock_tex);
    mock_tex.nvn_format = mode == 25 ? 0xdead : NVN_FORMAT_RGBA8;
    mock_tex.nvn_target = NVN_TEXTURE_TARGET_2D;
    mock_tex.view = (VkImageView)0x11;
    mock_tex.view_att = (VkImageView)0x12;
    if (why && cap) why[0] = 0;
    *tex_out = &mock_tex;
    // mode 26：编译等待期间对象被 Finalize/换掉——若消费点之后再解析，会拿到这个 0x9000 视图。
    *view_out = (VkImageView)(uintptr_t)((mutated ? 0x9000u : 0x1000u) + handle);
    *sampler_out = (VkSampler)0x22;
    return 0;
}
void s2a_vk_nvn_texture_release_for_compute(struct nvn_vk_tex *t, VkImageView v) {
    (void)t;
    if (v != (VkImageView)(uintptr_t)(0x1000 + bind_handles[0])) return;
    release_calls++;
}
static const char *good, *bad;
static struct s2a_direct_compile_context pc;
static uint8_t expected_identity[32];
static int read_bytes(const char *path,uint8_t **b,size_t *n) {
    FILE *f=fopen(path,"rb"); assert(f); fseek(f,0,SEEK_END);*n=(size_t)ftell(f);rewind(f);
    *b=malloc(*n);assert(*b&&fread(*b,1,*n,f)==*n);fclose(f);return 0;
}
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    provider_calls++;*out=pc;if(mode==9)return -1;if(mode==10&&provider_calls>1)out->generation++;return 0;
}
int s2a_vk_adapter_exec_pause(void) { pauses++;return mode==11?-1:0; }
void s2a_vk_adapter_exec_resume(void) { resumes++;if(mode==12)stopped=1; }
int s2a_vk_adapter_stop_requested(void) { return stopped; }
int s2a_offline_library_fetch(const char *root,const struct s2a_offline_request *q,
    struct s2a_offline_program *out,char *why,size_t cap) {
    assert(!strcmp(root,"/revision"));assert(q->profile_declared);offline++;
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
// The producer echoes the request's own requestId under result.transport.
// Fixtures carry a <RID> placeholder; the mock substitutes the live requestId
// exactly where a real compiler would, so accept() still verifies the echo.
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
int s2a_compile_worker_run(uint64_t session,uint64_t generation,const char *json,size_t n,
    const uint8_t id[32],int timeout,int(*pause)(void*),void(*resume)(void*),void *ctx,
    uint8_t **bytes,size_t *len,char *why,size_t cap) {
    (void)n;assert(session==11&&generation==22&&timeout>0);worker++;
    if(offline)assert(!memcmp(expected_identity,id,32));
    // 真实的纹理绑定事实必须进入 REQUEST（空表只允许在真的没有绑定时出现）。
    if (mode == 27) assert(strstr(json,"\"constantBufferUse\":56"));
    if (mode == 5) assert(strstr(json,"\"textures\":[{\"stage\":\"cs\",\"handle\":14,"));
    else if (mode == 23 || mode == 26) assert(strstr(json,"\"textures\":[{\"stage\":\"cs\",\"handle\":14,"));
    else assert(strstr(json,"\"textures\":[]"));
    int unaligned=mode==1||mode==2;
    assert(strstr(json,unaligned?"\"hasUnalignedStorageBuffer\":true":"\"hasUnalignedStorageBuffer\":false"));
    assert(pause&&resume);
    // Required worker contract, separately tested by worker suite: reject before start/wait.
    if(pause(ctx)){snprintf(why,cap,"pause refused");return -1;}
    s2a_source_clear();uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(held>0);
    if(mode==8||mode==14){snprintf(why,cap,"MISSING_FACT texture query");resume(ctx);return -1;}
    // mode 26：编译期间“对象被换掉”：此后任何按槽号重新解析都会看到另一个视图。
    if(mode==26)mutated=1;
    int rc=read_bytes(good,bytes,len);
    // 真编译器在结果封套 transport 里回显请求自己的 requestId；夹具用 <RID> 占位，
    // 这里按 REQUEST 顶层 requestId 原地回填——fresh 回显检查因此是对活身份做等价比对，
    // 不是夹具自证。
    if(!rc)*bytes=echo_request_id(*bytes,len,json);
    resume(ctx);return rc;
}
static int finish(struct s2a_vk_device *d,void *owner,int completion) {
    (void)d;assert(owner==(void*)42&&completion==-1);finished++;return 0;
}
int nvn_vk_compute_pipeline_build_prog(struct s2a_vk_device *device,const struct s2a_record_dispatch_state *s,
    const struct s2a_record_memory *memory,struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,
    const struct nvk_compute_lookup_result *p,const uint8_t identity[32],
    struct s2a_record_dispatch_output *out,char *why,size_t cap) {
    (void)device;(void)s;(void)memory;pipeline++;assert(p->bytes&&p->length&&p->profile.storage_alignment==16);
    assert(identity);   // miss 路径必须带 request.identity 供程序级入表（prog 门关时只是参数透传）
    // c1 常量段必须来自消费点 pin 住的源快照（不是按槽号回查）：这里断言同一次 prepare 的那份。
    src_seen = source != NULL;
    uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(held>0);
    // 消费点快照必须已经在这里：编译等待之后不再按槽号重新解析。
    captured_count = snapshot ? snapshot->count : -1;
    captured_view = snapshot && snapshot->count ? snapshot->items[0].view : VK_NULL_HANDLE;
    if(mode==15){snprintf(why,cap,"profile=16 device=32");return -1;}
    out->pipeline=(VkPipeline)1;out->layout=(VkPipelineLayout)2;out->owner=(void*)42;out->finish=finish;return 0;
}
/* 程序级缓存缝：本测试不链 compute.c，桩成「永不命中」——命中路径由 pipeline 测试验收。 */
struct cpso *nvn_vk_compute_prog_lookup(struct s2a_vk_device *d,const uint8_t identity[32]) {
    (void)d;(void)identity;return mode==31 ? (struct cpso *)(uintptr_t)1 : NULL;
}
void nvn_vk_compute_prog_release(struct cpso *entry) { (void)entry; }
int nvn_vk_compute_pipeline_build_cached(struct s2a_vk_device *d,const struct s2a_record_dispatch_state *s,
    const struct s2a_record_memory *memory,struct nvk_compute_tex_snapshot *snapshot,
    const struct s2a_source_entry_view *source,struct cpso *entry,
    struct s2a_record_dispatch_output *out,char *why,size_t cap) {
    (void)d;(void)s;(void)memory;(void)snapshot;(void)source;(void)entry;(void)out;
    if(mode==31){
        pipeline++;src_seen=source!=NULL;
        if(++cached_attempts==1)return S2A_RECORD_DISPATCH_RETRY_SYNC;
        out->pipeline=(VkPipeline)1;out->layout=(VkPipelineLayout)2;out->owner=(void*)42;out->finish=finish;return 0;
    }
    snprintf(why,cap,"cached stub never reached");return -1;
}
static void wr(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
// 诊断目录事实：后缀计数/取名/唯一请求集目录。门开时请求与结果各一件且同名配对；
// 门关时一件不落。文件名放不下（变异负控会把缓冲调小）时写作被跳过、计数随之转红。
static int diag_count(const char *dir,const char *suffix) {
    DIR *d=opendir(dir);if(!d)return 0;int n=0;struct dirent *e;
    while((e=readdir(d)))if(e->d_name[0]!='.'&&strstr(e->d_name,suffix))n++;
    closedir(d);return n;
}
static int diag_name(const char *dir,const char *suffix,char *out,size_t cap) {
    DIR *d=opendir(dir);if(!d)return -1;struct dirent *e;int rc=-1;
    while((e=readdir(d)))if(e->d_name[0]!='.'&&strstr(e->d_name,suffix)){snprintf(out,cap,"%s",e->d_name);rc=0;break;}
    closedir(d);return rc;
}
static int diag_sets(const char *dir,char *out,size_t cap) {
    DIR *d=opendir(dir);if(!d)return 0;int n=0;struct dirent *e;
    while((e=readdir(d)))if(!strncmp(e->d_name,"requests-",9)){n++;if(out)snprintf(out,cap,"%s/%s",dir,e->d_name);}
    closedir(d);return n;
}
int main(int argc,char **argv) {
    assert(argc==5);mode=atoi(argv[1]);good=argv[2];bad=argv[3];
    setenv("S2A_SHADER_REVISION_ROOT",mode==16?"relative":"/revision",1);
    setenv("S2A_DIRECT_CACHE_DIR",mode==16?"":"/private",1);
    setenv("S2A_DIRECT_DIAG_DIR",argv[4],1);
    const char *hash="sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *profile="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
    const char *closure="closure";
    // 诊断/唯一请求集回归要过 direct_batch.import_requests 的 validate_request：它要求
    // profile 带 directABI/translatorClosure/runtimeCompilerABI/supportLayout.hash 且与顶层
    // 一致（生产 profile 的真实形状），所以这套用例用完整 profile；其余用例保持最小 profile。
    if (getenv("FULL_EXPORT_PROFILE")) {
        closure="synthetic-closure-v1";
        profile="{\"schema\":\"s2a-direct-profile-v1\",\"directABI\":1,"
            "\"translatorClosure\":\"synthetic-closure-v1\",\"runtimeCompilerABI\":\"s2a-direct-compile-v1\","
            "\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
            "\"supportLayout\":{\"hash\":\"sha256-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},"
            "\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
    }
    pc=(struct s2a_direct_compile_context){.session=11,.generation=22,.direct_abi=1,
        .translator_closure=closure,.profile_hash=hash,.profile_json=profile,.profile_bytes=strlen(profile),.offline_profile_available=1};
    uint8_t ctl[0x760]={0},code[16]={1,2,3,4},digest[32];
    wr(ctl+0x20,0x760);wr(ctl+0x748,8);wr(ctl+0x74c,1);wr(ctl+0x750,1);
    s2a_sha256 h;s2a_sha256_init(&h);s2a_sha256_update(&h,ctl,sizeof ctl);s2a_sha256_update(&h,code,sizeof code);s2a_sha256_final(&h,digest);
    struct s2a_record_dispatch_state state={.stages=32,.tex_pool_seen=1,.samp_pool_seen=1,.tex_pool_n=7,.samp_pool_n=9};
    memcpy(state.keys[0],digest,8);assert(!s2a_source_keep(state.keys[0],5,code,sizeof code,ctl,sizeof ctl));
    state.ssbo_seen=1u<<7;state.ssbo[7].addr=0x1100;state.ssbo[7].size=16;
    if(mode==31)state.fq_pool=1;
    if(mode==1)state.ssbo[7].addr++;
    if(mode==2){state.ssbo_seen|=1u<<15;state.ssbo[15].addr=0x2001;state.ssbo[15].size=4;}
    if(mode==3){state.ssbo[15].addr=0x2001;state.ssbo[15].size=4;}
    if(mode==4){state.ssbo_seen|=1u<<15;state.ssbo[15].addr=0x2001;state.ssbo[15].size=0;}
    if(mode==5)state.tex_present[3]=1;if(mode==6)state.image_present[4]=1;if(mode==7)state.samp_present[5]=1;
    // mode 24 要验“取快照期间注册表换代”：必须真有一条纹理绑定让快照去查 reggen；
    // 旧夹具漏了这行（无纹理绑定 → 根本不取快照 → 竞态检查不触发，mode 24 被当成正常成功）。
    if(mode==24){state.tex_present[0]=1;state.tex_handle[0]=7;}
    if(mode==21){state.tex_present[0]=1;state.tex_handle[0]=99;}
    if(mode==22){state.tex_present[0]=1;state.tex_handle[0]=7;}
    if(mode==25){state.tex_present[0]=1;state.tex_handle[0]=7;}
    // 同一槽位换对象：两次 dispatch 的 handle 不同，第二次必须解析/绑定到新对象，不能拿旧快照。
    if(mode==23){state.tex_present[3]=1;state.tex_handle[3]=((uint64_t)11|((uint64_t)2<<20));}
    if(mode==26){state.tex_present[3]=1;state.tex_handle[3]=((uint64_t)11|((uint64_t)2<<20));}
    // 27：compute 用户槽 2（cbuf 5）——旧 5 位窗口外的位必须能走完整条链；
    // 28：位 19（cbuf 19，超出可表达集合）仍 fail-closed。
    if(mode==27)state.ubo_use[5]=0x18|(1u<<5);
    if(mode==28)state.ubo_use[5]=0x18|(1u<<19);
    if(mode==17){pc.profile_json="{}";pc.profile_bytes=2;}
    if(mode==18)s2a_source_clear();if(mode==19)state.tex_pool_seen=0;if(mode==20)stopped=1;
    struct s2a_vk_device device={0};struct s2a_record_memory memory={0};
    struct s2a_record_dispatch_output out={0};struct s2a_record_result result={.executed=123};
    int rc=nvn_vk_compute_build(&device,&state,&memory,&out,&result,NULL);
    if(mode==31 && cached_attempts==1){assert(rc==S2A_RECORD_DISPATCH_RETRY_SYNC && pipeline==1 && acquired==1 && released==1);
        assert(!out.owner&&!out.pipeline);puts("cached retry: exact sync-required status and source release PASS");return 0;}
    int success=mode<=7||mode==13||mode==16||mode==23||mode==26||mode==27||mode==29||mode==30||mode==31;
    if(success){assert(rc==0&&pipeline==(mode==31?2:1)&&out.finish&&finished==0);out.finish(&device,out.owner,-1);assert(finished==1);}
    else {assert(rc!=0&&result.code==rc&&result.why[0]&&result.opcode==OP_DISPATCH&&result.executed==123);assert(!out.owner&&!out.pipeline);}
    // 6/7：独立图像/独立采样器“存在”不是 shader 查询它们的证明——不拦，交给编译器反射与
    // pipeline 的 descriptor 分支明确失败；这里只证明不再因为“绑过”就整笔拒绝。
    if(mode==21)assert(!offline&&!worker&&!pipeline&&strstr(result.why,"compute texture fact unresolved: slot=0 id=99")&&strstr(result.why,"not registered"));
    if(mode==22)assert(!offline&&!worker&&!pipeline&&strstr(result.why,"compute texture fact unmapped: slot=0 id=7")&&strstr(result.why,"target=0xdead"));
    // 注册表在取快照期间换代 → 整笔拒绝（两份不同的对象不能混成一条快照）。
    // 合并后实现改成“换代后逐项复核”：快照已 pin 一条（bind_calls==1），复核发现对象换了才拒绝；
    // 旧断言 bind_calls==0 是换代即失败的旧实现口径。
    if(mode==24)assert(!offline&&!worker&&!pipeline&&strstr(result.why,"compute texture snapshot raced a registry change: reggen 7 -> 8")&&bind_calls==1&&release_calls==1);
    // 解析出的对象与注册表事实不一致（同槽换对象/竞争）→ 拒绝，且取消已登记的保活。
    if(mode==25)assert(!offline&&!worker&&!pipeline&&strstr(result.why,"compute texture snapshot is inconsistent: slot=0 id=7")&&strstr(result.why,"object fmt=0xdead"));
    if(mode==28)assert(!offline&&!worker&&!pipeline&&strstr(result.why,"constant-buffer mask: stages=0x20 ubo_use=0x80018 overflow=0"));
    if(mode==8||mode==14)assert(worker==1&&pipeline==0&&strstr(result.why,"MISSING_FACT"));
    if(mode==11)assert(pauses==1&&resumes==0&&pipeline==0);
    if(mode==12)assert(pauses==1&&resumes==1&&pipeline==0&&strstr(result.why,"stopped"));
    if(mode==13)assert(offline==1&&disk==0&&worker==0&&pauses==0);
    if(mode==16)assert(!offline&&!disk&&worker==1);
    if((mode>=17&&mode<=20)||mode==9||mode==10||mode==21||mode==22||mode==24||mode==25||mode==28)assert(!worker&&!pipeline);
    assert(offline<=1&&disk<=1&&worker<=1&&acquired<=(mode==31?2:1)&&released<=(mode==31?2:1));
    if(pipeline)assert(src_seen);   // c1 由消费点源快照提供，pipeline 必须收到它
    if(success||worker||mode==10||mode==15)assert(acquired==(mode==31?2:1)&&released==(mode==31?2:1));
    if(mode==5){
        assert(bind_calls==1&&captured_count==1&&captured_view==(VkImageView)(uintptr_t)0x1000&&release_calls==1);
        printf("snapshot: count=1 view=0x%llx sampler=0x22 handle=0x%llx; request carries handleIndex 14; pin released after finish\n",
               (unsigned long long)(uintptr_t)captured_view, (unsigned long long)bind_handles[0]);
    }
    // 反例②：编译等待期间对象被换掉（worker 里置 mutated=1）。恢复后必须仍绑消费点快照里的那份
    // （0x1000 视图），且全程只解析一次；按槽号重新解析会拿到 0x9000 视图。
    if(mode==26){
        const uint64_t handle = (uint64_t)11|((uint64_t)2<<20);
        assert(bind_calls==1&&mutated==1&&captured_count==1);
        assert(captured_view==(VkImageView)(uintptr_t)(0x1000+handle));
        printf("compile-wait replacement: bound view=0x%llx (re-resolution after compile would be 0x%llx), resolve calls=%d\n",
               (unsigned long long)(uintptr_t)captured_view, (unsigned long long)(0x9000u+handle), bind_calls);
    }
    if(mode==23){
        // 第二次 dispatch：同一槽位换成另一个对象。编译等待之后仍只认消费点快照里的那份。
        const uint64_t second = ((uint64_t)33|((uint64_t)5<<20));
        state.tex_handle[3]=second;
        // 第一次编译把源快照清掉了；第二次 dispatch 之前重新登记同一份源（真实路径每次都会重新 pin）。
        assert(!s2a_source_keep(state.keys[0],5,code,sizeof code,ctl,sizeof ctl));
        pipeline=0;worker=0;offline=0;disk=0;finished=0;bind_calls=0;
        struct s2a_record_dispatch_output out2={0};struct s2a_record_result result2={.executed=7};
        int rc2=nvn_vk_compute_build(&device,&state,&memory,&out2,&result2,NULL);
        assert(rc2==0&&pipeline==1&&out2.finish&&captured_count==1);
        assert(bind_calls==1&&bind_handles[0]==second);
        assert(captured_view==(VkImageView)(uintptr_t)(0x1000+second));
        out2.finish(&device,out2.owner,-1);
        assert(release_calls>=1);
        printf("bridge rebind: slot 3 handle 0x%llx -> 0x%llx, snapshot view 0x%llx\n",
               (unsigned long long)((uint64_t)11|((uint64_t)2<<20)), (unsigned long long)second,
               (unsigned long long)(0x1000+second));
    }
    s2a_source_clear();uint32_t entries,held,dropped;s2a_source_stats(&entries,&held,&dropped);assert(held==0);
    // 诊断合同（与图形 compile-request 同门同目录、主线 F4.1）：
    // 门开 = 请求 <requestId>-<身份64hex>.compile-request.json 与结果 <同一requestId>-<同一身份>.compile-result.json
    // 各一件、同名配对；唯一请求集 requests-<s>-<g>-<cohort>/ 下同名同字节一份（批编导入按此正则）。
    if(mode==0){
        char req[320]={0},res[320]={0},set[512]={0};
        assert(diag_count(argv[4],".compile-request.json")==1);
        assert(diag_count(argv[4],".compile-result.json")==1);
        assert(!diag_name(argv[4],".compile-request.json",req,sizeof req));
        assert(!diag_name(argv[4],".compile-result.json",res,sizeof res));
        static const char rq[]=".compile-request.json", rs[]=".compile-result.json";
        assert(strlen(req)>sizeof rq-1&&strlen(res)>sizeof rs-1);
        assert(!strcmp(req+strlen(req)-(sizeof rq-1),rq));
        assert(!strcmp(res+strlen(res)-(sizeof rs-1),rs));
        assert(strlen(req)-(sizeof rq-1)==strlen(res)-(sizeof rs-1));
        assert(!strncmp(req,res,strlen(req)-(sizeof rq-1)));
        static const char hex[]="0123456789abcdef";char want[320];size_t w=0;
        w+=(size_t)snprintf(want+w,sizeof want-w,"cs-11-22-0-");
        for(int i=0;i<32;i++){want[w++]=hex[out.diagnostic.identity[i]>>4];want[w++]=hex[out.diagnostic.identity[i]&15];}
        snprintf(want+w,sizeof want-w,".compile-request.json");
        assert(!strcmp(req,want));
        assert(diag_sets(argv[4],set,sizeof set)==1);
        char reqpath[1200],exported[1200];snprintf(reqpath,sizeof reqpath,"%s/%s",argv[4],req);
        snprintf(exported,sizeof exported,"%s/%s",set,req);
        uint8_t *a=NULL,*b=NULL;size_t an=0,bn=0;
        assert(!read_bytes(reqpath,&a,&an)&&!read_bytes(exported,&b,&bn));
        assert(an==bn&&!memcmp(a,b,an));free(a);free(b);
        printf("diag paired: %s\n",req);
    }
    // 门关（mode29）：请求、结果、唯一请求集一件都不落——正式包缺省行为。
    if(mode==29)assert(diag_count(argv[4],".compile-request.json")==0&&diag_count(argv[4],".compile-result.json")==0&&diag_sets(argv[4],NULL,0)==0);
    // 发布失败（mode30：诊断根落在普通文件下）：只丢诊断并打红，结果判定不变（success 已断言）。
    if(mode==30)assert(logcalls>0&&strstr(logall,"【红】唯一请求集发布失败")&&diag_count(argv[4],".compile-request.json")==0);
    printf("bridge case%d PASS lookup=%d/%d/%d pipeline=%d pause/resume=%d/%d\n",mode,offline,disk,worker,pipeline,pauses,resumes);return 0;
}
