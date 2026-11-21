// SPDX-License-Identifier: Apache-2.0
// 真 rp_dispatch → rp_draw → Vulkan。计算写间接参数，图形通道内读取。
// VVL 1.4.341 + syncval；旧校验层看不出计算 SSBO 写，不能作为修前红。
// automatic: 被测生产路径；manual: 外置完整屏障控制；no-producer: 无计算写控制。
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <stdatomic.h>
static nvn_gpu_err err;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
static int disable_compute_access;
static int dq_mode;
int nvn_plat_flag(const char *n,int d){
 if(!strcmp(n,"nvk_cgr"))return !disable_compute_access;
 if(!strcmp(n,"nvk_dq")){const char *e=getenv("S2A_COMPUTE_READ_DQ");REQUIRE(!e||!strcmp(e,"0")||!strcmp(e,"1"));dq_mode=e?atoi(e):d;return dq_mode;}
 return d;
}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static void done(void *u,const nvn_gpu_done *d){(void)u;(void)d;}
void rp_buf_reset(void);
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout){
 char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
 FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
 void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
 nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
 nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
// 受控替身：程序来源、通道、池登记。镜像分配、GPU 写区间排除、地址解析、派发与 Vulkan 都是真的。
int g_s2a_logfd=2,s2a_nvn_rast_flip;
static alignas(4096) unsigned char guest[4096];
static struct rp_prog prog,graphics;static uint32_t pending;
int rp_shader_resolve_compute(const struct rp_prog **o){*o=&prog;return 0;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&graphics;return 0;}
void rp_pass_barrier(uint32_t bits){pending|=bits;}
uint32_t rp_pass_take_barrier(void){uint32_t b=pending;pending=0;return b;}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
uint64_t rp_frame(void){return 1;}
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return 1;}
static uint64_t serial=1;
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return serial;}
int rp_fence_done(uint64_t f){return f<=nvn_gpu_fence_done();}
static nvn_gpu_cmd active;
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_flush(active);*last=serial;return active;}
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
void rp_seg_prog_note(const struct rp_prog *p) { (void)p; }   // rp_exec.c 的段失败键记录：本用例不验
// GPU 写回登记：同 rp_exec.c 交给 rp_buf 的排除表；收尾不回写游戏内存
static uint64_t wkey[4],wid[4];static int nw;
int rp_post_gpu_write(uint64_t a,uint64_t n){if(nw>=4){return 0;}wid[nw]=rp_buf_gpu_write_note(a,n,&wkey[nw]);if(!wid[nw]){return 0;}nw++;return 1;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
// 受写跟踪且 CPU 没再改过的池：首次解析直接写镜像，之后不录传输或全屏障，不能掩盖计算写的依赖。
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return 1;}
static const int untracked=0;
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return untracked;}
const void *nvn_dirty_span(const void *a,size_t n){(void)n;return untracked?NULL:a;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;if(untracked){return -1;}for(size_t i=0;i<n;i++){g[i]=1;}return (long)n;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
// 本夹具只模拟逐页代号，没有追加日志或真实写保护；返回不可用，不能
// 假报日志为空/页面已武装。resources 夹具 include 本文件，使用同一套语义。
int nvn_dirty_log_open(const void *s,uint64_t *id,uint64_t *cursor){(void)s;(void)id;(void)cursor;return 0;}
int nvn_dirty_log_read(const void *s,uint64_t id,uint64_t *cursor,void (*fn)(void *,uintptr_t,uintptr_t),void *user){(void)s;(void)id;(void)cursor;(void)fn;(void)user;return 0;}
int nvn_dirty_page_armed(const void *s,const void *a){(void)s;(void)a;return 0;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
static struct { tex_t tex; view_t desc; nvn_gpu_view view; int valid, has_view; } resources[4];
static nvn_gpu_samp resource_sampler;
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)c;if(t)memset(t,0,sizeof *t);if(i<0||i>=4||!resources[i].valid)return NULL;if(t)*t=resources[i].tex;return resources[i].view;}   // #1627 起调用方给快照：取到拷贝、没取到清零
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)t;(void)k;(void)e;return rp_view_by_id(i,c,o);}
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
// 本夹具不提供格式重解释；不认识的视图严格拒绝。
nvn_gpu_view rp_view_for(const tex_t *t,const view_t *v,nvn_gpu_cmd c){(void)t;(void)v;(void)c;return NULL;}
uint32_t rp_storage_srgb_alias(uint32_t f){(void)f;return 0;}
nvn_gpu_samp rp_samp_by_id(int i){return i==1?resource_sampler:NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid){(void)tid;return rp_samp_by_id(sid);}
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn){(void)pb;(void)pn;return rp_samp_for_tex(sid,tid);}   // 同 rp_samp_for_tex：替身不给默认采样器
// 夹具不提供查询采样器；请求该能力时返回不可用。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f){return nvn_gpu_format_caps(f);}
int rp_tex_snap_by_id(int i,tex_t *t,view_t *v,int *h){if(i<0||i>=4||!resources[i].valid)return 0;*t=resources[i].tex;*v=resources[i].desc;*h=resources[i].has_view;return 1;}
uint64_t rp_tex_reg_gen(void){return 1;}uint64_t rp_tex_fact_sig(const uint64_t *h,int n,int b,int c){(void)h;(void)n;(void)b;(void)c;return 0;}
void rp_tex_mark_gpu_written(const tex_t *t){(void)t;}int rp_tex_debug_state(uint64_t s){(void)s;return 0;}
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s){(void)s;return NULL;}
static p_set_rt target;static tex_t target_tex;static nvn_gpu_view target_view;static int pass_open;
const p_set_rt *rp_pass_target(void){return &target;}int rp_pass_slot_attached(int i){return i==0;}
void rp_pass_slot_size(int i,uint32_t *w,uint32_t *h){(void)i;*w=*h=64;}void rp_pass_noatt_size(uint32_t *w,uint32_t *h){(void)w;(void)h;}void rp_pass_note_draw(int d){(void)d;}
int rp_pass_parked(void){return 0;}void rp_pass_prepare_draw(nvn_gpu_cmd c,uint32_t color_write_mask){(void)c;(void)color_write_mask;}void rp_pass_land_reads(nvn_gpu_cmd c,const tex_t *const *ts,int n){(void)c;(void)ts;(void)n;}
static void pass(nvn_gpu_cmd c,int clear){
 nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=target_view;
 p.color[0].load=clear?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD;p.color[0].store=NVN_GPU_STORE_STORE;
 p.color[0].clear.f[3]=1;p.w=p.h=64;p.layers=1;nvn_gpu_pass_begin(c,&p);pass_open=1;
}
int rp_pass_begin(nvn_gpu_cmd c){if(!pass_open)pass(c,0);return 1;}
void rp_pass_flush(nvn_gpu_cmd c){if(pass_open){nvn_gpu_pass_end(c);pass_open=0;}}
// Vulkan 调用观察器：动态入口与直接符号均转发真函数，不能把未支持入口伪造成可用。
static int explicit_settings=1,trace_window,normalize_dependencies,cross_segment;
static unsigned nbar,ndispatch,ndraw,ncompute_bar;
// Only the real vkCmd observer writes these. With DQ enabled it runs on the
// recorder thread; inspect snapshots after the normal final commit/fence, never
// insert a drain or GPU wait between the producer and its consumers.
static unsigned dispatch_bar[4],dispatch_compute_bar[4],draw_bar[4],draw_compute_bar[4];
static void note_draw(void){REQUIRE(ndraw<4);draw_bar[ndraw]=nbar;draw_compute_bar[ndraw]=ncompute_bar;ndraw++;}
static _Atomic unsigned submits,waits,idles;
static _Thread_local int encoding_thread;
static unsigned encoding_waits,start_encoding_waits;
static unsigned start_submits,start_waits,start_idles;
static VkCommandBuffer producer_cb;
VkResult __real_vkCreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*);
VkResult __wrap_vkCreateInstance(const VkInstanceCreateInfo *ci,const VkAllocationCallbacks *alloc,VkInstance *out){
 if(!explicit_settings)return __real_vkCreateInstance(ci,alloc,out);
 VkBool32 yes=VK_TRUE;VkLayerSettingEXT values[]={
  {"VK_LAYER_KHRONOS_validation","validate_sync",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&yes},
  {"VK_LAYER_KHRONOS_validation","syncval_shader_accesses_heuristic",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&yes}
 };
 VkLayerSettingsCreateInfoEXT settings={.sType=VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,.pNext=ci->pNext,.settingCount=2,.pSettings=values};
 VkInstanceCreateInfo copy=*ci;copy.pNext=&settings;return __real_vkCreateInstance(&copy,alloc,out);
}
VkResult __real_vkQueueSubmit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence);
VkResult __wrap_vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *s,VkFence f){atomic_fetch_add(&submits,1);return __real_vkQueueSubmit(q,n,s,f);}
VkResult __real_vkWaitForFences(VkDevice,uint32_t,const VkFence*,VkBool32,uint64_t);
VkResult __wrap_vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout){atomic_fetch_add(&waits,1);if(encoding_thread)encoding_waits++;printf("TRACE wait role=%s timeout_ns=%llu\n",encoding_thread?"encoding":"completion",(unsigned long long)timeout);return __real_vkWaitForFences(d,n,f,all,timeout);}
VkResult __real_vkQueueWaitIdle(VkQueue);
VkResult __wrap_vkQueueWaitIdle(VkQueue q){atomic_fetch_add(&idles,1);return __real_vkQueueWaitIdle(q);}
VkResult __real_vkDeviceWaitIdle(VkDevice);
VkResult __wrap_vkDeviceWaitIdle(VkDevice d){atomic_fetch_add(&idles,1);return __real_vkDeviceWaitIdle(d);}
VkResult __real_vkCreateRenderPass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass*);
VkResult __wrap_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *ci,const VkAllocationCallbacks *a,VkRenderPass *p){
 // 仅测试：按 v1.4.341 §8.2 的等价定义展开 ALL_COMMANDS；不是删除依赖，也不是生产改动。
 VkRenderPassCreateInfo copy=*ci;VkSubpassDependency *deps=NULL;
 if(normalize_dependencies&&ci->dependencyCount){
  deps=calloc(ci->dependencyCount,sizeof *deps);REQUIRE(deps);memcpy(deps,ci->pDependencies,ci->dependencyCount*sizeof *deps);
  for(uint32_t i=0;i<ci->dependencyCount;i++){
   if(deps[i].srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)deps[i].srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
   if(deps[i].dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)deps[i].dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
  }copy.pDependencies=deps;ci=&copy;
 }
 VkResult result=__real_vkCreateRenderPass(d,ci,a,p);if(result==VK_SUCCESS)for(uint32_t i=0;i<ci->dependencyCount;i++){
  const VkSubpassDependency *b=&ci->pDependencies[i];
  printf("TRACE pass=%p dep=%u subpass=%u->%u stage=%#x->%#x access=%#x->%#x\n",(void*)*p,i,b->srcSubpass,b->dstSubpass,b->srcStageMask,b->dstStageMask,b->srcAccessMask,b->dstAccessMask);
 }free(deps);return result;
}
void __real_vkUpdateDescriptorSets(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*);
void __wrap_vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t nc,const VkCopyDescriptorSet *copies){
 for(uint32_t i=0;i<n;i++)if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&w[i].pBufferInfo){
  const VkDescriptorBufferInfo *b=w[i].pBufferInfo;printf("TRACE ssbo set=%p binding=%u buffer=%p offset=%llu range=%llu\n",(void*)w[i].dstSet,w[i].dstBinding,(void*)b->buffer,(unsigned long long)b->offset,(unsigned long long)b->range);
 }__real_vkUpdateDescriptorSets(d,n,w,nc,copies);
}
PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice,const char*);
static PFN_vkCmdPipelineBarrier real_barrier;
static PFN_vkCmdDispatch real_dispatch;
static PFN_vkCmdDrawIndirect real_draw;
static PFN_vkCmdDrawIndexedIndirect real_indexed_draw;
static PFN_vkCmdDraw real_direct_draw;
static PFN_vkCmdDrawIndexed real_direct_indexed_draw;
static PFN_vkCmdBeginRenderPass real_begin;
static void VKAPI_CALL count_barrier(VkCommandBuffer cb,VkPipelineStageFlags s,VkPipelineStageFlags t,VkDependencyFlags f,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im){
 nbar++;
 if(s==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&nm==1&&m[0].srcAccessMask==VK_ACCESS_SHADER_WRITE_BIT&&(t&VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT)&&(m[0].dstAccessMask&VK_ACCESS_INDIRECT_COMMAND_READ_BIT))ncompute_bar++;
 if(trace_window){printf("TRACE barrier cb=%p stage=%#x->%#x memory=%u buffer=%u image=%u\n",(void*)cb,s,t,nm,nb,ni);for(uint32_t i=0;i<nm;i++)printf("TRACE memory access=%#x->%#x\n",m[i].srcAccessMask,m[i].dstAccessMask);}
 real_barrier(cb,s,t,f,nm,m,nb,b,ni,im);
}
static void VKAPI_CALL count_dispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z){
 REQUIRE(ndispatch<4);dispatch_bar[ndispatch]=nbar;dispatch_compute_bar[ndispatch]=ncompute_bar;
 ndispatch++;producer_cb=cb;start_submits=atomic_load(&submits);start_waits=atomic_load(&waits);start_encoding_waits=encoding_waits;start_idles=atomic_load(&idles);trace_window=1;
 printf("TRACE dispatch cb=%p groups=%u,%u,%u\n",(void*)cb,x,y,z);real_dispatch(cb,x,y,z);
}
static void VKAPI_CALL count_draw(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize off,uint32_t count,uint32_t stride){
 note_draw();printf("TRACE draw cb=%p buffer=%p offset=%llu count=%u stride=%u same_cb=%d submit_delta=%u wait_delta=%u idle_delta=%u encoding_wait_delta=%u\n",(void*)cb,(void*)buffer,(unsigned long long)off,count,stride,producer_cb==cb,atomic_load(&submits)-start_submits,atomic_load(&waits)-start_waits,atomic_load(&idles)-start_idles,encoding_waits-start_encoding_waits);
 if(ndispatch){if(!cross_segment)REQUIRE(cb==producer_cb);REQUIRE(atomic_load(&submits)-start_submits==(unsigned)cross_segment);REQUIRE(encoding_waits==start_encoding_waits);REQUIRE(atomic_load(&idles)==start_idles);}
 real_draw(cb,buffer,off,count,stride);trace_window=0;
}
static void VKAPI_CALL count_indexed_draw(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize off,uint32_t count,uint32_t stride){
 note_draw();printf("TRACE indexed_draw cb=%p buffer=%p offset=%llu count=%u stride=%u same_cb=%d\n",(void*)cb,(void*)buffer,(unsigned long long)off,count,stride,producer_cb==cb);
 REQUIRE(ndispatch&&cb==producer_cb);REQUIRE(atomic_load(&submits)==start_submits);REQUIRE(encoding_waits==start_encoding_waits);REQUIRE(atomic_load(&idles)==start_idles);
 real_indexed_draw(cb,buffer,off,count,stride);trace_window=0;
}
static void check_direct_draw(VkCommandBuffer cb,uint32_t count,uint32_t instances,int indexed){
 note_draw();printf("TRACE direct_draw indexed=%d cb=%p count=%u instances=%u same_cb=%d\n",indexed,(void*)cb,count,instances,producer_cb==cb);
 REQUIRE(ndispatch&&cb==producer_cb);REQUIRE(atomic_load(&submits)==start_submits);REQUIRE(encoding_waits==start_encoding_waits);REQUIRE(atomic_load(&idles)==start_idles);trace_window=0;
}
static void VKAPI_CALL count_direct_draw(VkCommandBuffer cb,uint32_t count,uint32_t instances,uint32_t first,uint32_t base){if(trace_window)check_direct_draw(cb,count,instances,0);real_direct_draw(cb,count,instances,first,base);}
static void VKAPI_CALL count_direct_indexed_draw(VkCommandBuffer cb,uint32_t count,uint32_t instances,uint32_t first,int32_t voff,uint32_t base){if(trace_window)check_direct_draw(cb,count,instances,1);real_direct_indexed_draw(cb,count,instances,first,voff,base);}
static void VKAPI_CALL count_begin(VkCommandBuffer cb,const VkRenderPassBeginInfo *b,VkSubpassContents contents){printf("TRACE begin cb=%p pass=%p\n",(void*)cb,(void*)b->renderPass);real_begin(cb,b,contents);}
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice dev,const char *name){
 PFN_vkVoidFunction f=__real_vkGetDeviceProcAddr(dev,name);if(!f||!name)return f;
 if(!strcmp(name,"vkCmdPipelineBarrier")){real_barrier=(PFN_vkCmdPipelineBarrier)f;return (PFN_vkVoidFunction)count_barrier;}
 if(!strcmp(name,"vkCmdDispatch")){real_dispatch=(PFN_vkCmdDispatch)f;return (PFN_vkVoidFunction)count_dispatch;}
 if(!strcmp(name,"vkCmdDrawIndirect")){real_draw=(PFN_vkCmdDrawIndirect)f;return (PFN_vkVoidFunction)count_draw;}
 if(!strcmp(name,"vkCmdDrawIndexedIndirect")){real_indexed_draw=(PFN_vkCmdDrawIndexedIndirect)f;return (PFN_vkVoidFunction)count_indexed_draw;}
 if(!strcmp(name,"vkCmdDraw")){real_direct_draw=(PFN_vkCmdDraw)f;return (PFN_vkVoidFunction)count_direct_draw;}
 if(!strcmp(name,"vkCmdDrawIndexed")){real_direct_indexed_draw=(PFN_vkCmdDrawIndexed)f;return (PFN_vkVoidFunction)count_direct_indexed_draw;}
 if(!strcmp(name,"vkCmdBeginRenderPass")){real_begin=(PFN_vkCmdBeginRenderPass)f;return (PFN_vkVoidFunction)count_begin;}
 return f;
}
int main(int argc,char **argv){
 encoding_thread=1;
 REQUIRE(argc==3);
 const char *arm=argv[2];const int manual=!strcmp(arm,"manual")||!strcmp(arm,"manual-normalized"),no_producer=!strcmp(arm,"no-producer");
 const int unrelated=!strcmp(arm,"unrelated-normalized"),repeat=!strcmp(arm,"repeat-normalized"),zero=!strcmp(arm,"zero-normalized");
 const int game=!strcmp(arm,"game-barrier-normalized"),rewrite=!strcmp(arm,"rewrite-normalized"),readonly=!strcmp(arm,"readonly-normalized");
 disable_compute_access=!strcmp(arm,"disabled-normalized");
 cross_segment=!strcmp(arm,"cross-segment-normalized");
 explicit_settings=strcmp(arm,"automatic-env")!=0;normalize_dependencies=strstr(arm,"normalized")!=NULL;
 REQUIRE(manual||no_producer||!strcmp(arm,"automatic")||!explicit_settings||!strcmp(arm,"automatic-normalized")||unrelated||repeat||zero||disable_compute_access||cross_segment||game||rewrite||readonly);
 struct s2a_vk_device d;char why[256];if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 printf("device: %s (%s)\n",d.properties.deviceName,d.driver_name);
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-compute-read";GPU(nvn_gpu_open(&od,&err));
 prog.is_compute=1;prog.nent[0]=1;
 nvn_gpu_layout_ent *e=&prog.ent[0][0];e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;
 e->src_reg=0;e->src_reg2=UINT16_MAX;e->access=readonly?NVN_GPU_ACC_R:NVN_GPU_ACC_W;e->count=1;e->stages=1u<<NVN_GPU_SH_CS;
 nvn_gpu_layout lay={0};lay.ent=e;lay.nent=1;prog.sh[0]=shader(argv[1],readonly?"compute_read_readonly.comp":"compute_read_args.comp",NVN_GPU_SH_CS,&lay);
 graphics.sh[0]=shader(argv[1],"fullscreen.vert",NVN_GPU_SH_VS,NULL);
 graphics.sh[1]=shader(argv[1],"solid_green.frag",NVN_GPU_SH_FS,NULL);graphics.color_out_mask=15;
 // 参数由规格独立给出：一个覆盖屏幕的三角形，实例 1，firstVertex/firstInstance 0。
 const uint32_t expected[4]={3,1,0,0};memset(guest,0,sizeof guest);
 if(no_producer||zero||readonly)memcpy(guest,expected,sizeof expected);
 memcpy(guest+64,expected,sizeof expected);
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.ssbo[5][0].addr=(uint64_t)(uintptr_t)guest;rp_S.ssbo[5][0].size=sizeof expected;
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;
 td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));
 target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 // 所有初始化和清除都在 producer 之前；图形程序不读 DRV0，避免间接 firstInstance 的拷贝屏障假绿。
 nvn_gpu_buf mirror=NULL;uint64_t off=0;REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,sizeof expected,&mirror,&off));
 pass(active,1);rp_pass_flush(active);
 if(!no_producer){p_dispatch dp={0};dp.x=dp.y=dp.z=1;if(zero)dp.x=0;rp_dispatch(active,&dp,NULL);}
 if(cross_segment){GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));serial++;GPU(nvn_gpu_cmd_begin(NULL,&active,&err));}
 if(game)rp_pass_barrier(NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);
 if(manual)nvn_gpu_barrier(active,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);
 p_draw draw={0};draw.prim=4;draw.ind=(uint64_t)(uintptr_t)guest;
 if(unrelated){draw.ind=(uint64_t)(uintptr_t)(guest+64);rp_draw(active,&draw,NULL,&draw);draw.ind=(uint64_t)(uintptr_t)guest;}
 rp_draw(active,&draw,NULL,&draw);
 if(repeat)rp_draw(active,&draw,NULL,&draw);
 if(rewrite){rp_pass_barrier(NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);p_dispatch again={0};again.x=again.y=again.z=1;rp_dispatch(active,&again,NULL);rp_draw(active,&draw,NULL,&draw);}
 unsigned expected_draws=1u+(unsigned)(unrelated||repeat||rewrite);
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);
 REQUIRE(draws==expected_draws&&skips==0);REQUIRE(!nvn_unimpl_kinds());
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);
 GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));
 REQUIRE(ndraw==expected_draws&&ndispatch==(unsigned)(!no_producer&&!zero)+(unsigned)rewrite);
 unsigned before=ndispatch?dispatch_bar[0]:draw_bar[0],after=draw_bar[ndraw-1],compute_after=draw_compute_bar[ndraw-1];
 if(no_producer||zero||readonly||game||disable_compute_access){REQUIRE(compute_after==0);}
 else if(!manual){REQUIRE(compute_after==1u+(unsigned)rewrite);}
 if(unrelated)REQUIRE(draw_compute_bar[0]==dispatch_compute_bar[0]);
 if(repeat)REQUIRE(draw_compute_bar[0]==draw_compute_bar[1]);
 if(game)REQUIRE(after-before==1);
 const uint32_t *args=(const void*)((const uint8_t*)nvn_gpu_buf_cpu(mirror)+off);REQUIRE(!memcmp(args,expected,sizeof expected));
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){
  const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=255||p[2]||p[3]!=255)bad++;
 }
 printf("COMPUTE_READ arm=%s dq=%d draws=%llu args=3,1,0,0 pixel_mismatches=%u barriers_between=%u compute_barriers=%u\n",arm,dq_mode,(unsigned long long)draws,bad,after-before,compute_after);
 for(int k=0;k<nw;k++)rp_buf_gpu_write_land(wkey[k],wid[k],0);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(graphics.sh[0]);nvn_gpu_shader_release(graphics.sh[1]);
 nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);
 GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
// 页版本真实稳定，未知块代号不能伪造块命中。
int nvn_dirty_block_snapshot(const void *s,const void *a,size_t n,uint64_t *v,uint64_t *e){(void)s;(void)a;(void)n;(void)v;(void)e;return 0;}
