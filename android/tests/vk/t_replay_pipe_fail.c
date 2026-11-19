// SPDX-License-Identifier: Apache-2.0
// 建管线失败的处理（真 rp_draw / rp_buf / 后端 + lavapipe）：驱动失败时留在输出里的句柄不销毁、不绑定；红行带 VkResult / 程序身份 / 阶段键 / 配方，按配方限频。
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_plat.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <time.h>
extern uint64_t nvn_vk_dbg_dq_recs(void);
static nvn_gpu_err err;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){if(!strcmp(n,"nvk_opt_layout"))return !!getenv("S2A_REPLAY_INDIRECT_OPT_LAYOUT");return !strcmp(n,"nvk_dq")?!!getenv("S2A_REPLAY_INDIRECT_DQ"):d;}
static char g_red[16][1024];static int g_nred;void nvn_plat_log(int l,const char *c,const char *s){fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");if(l==NVN_PLAT_LOG_RED&&c&&!strcmp(c,"gpu")&&s&&strstr(s,"管线建不出")){if(g_nred<16)snprintf(g_red[g_nred],sizeof g_red[0],"%s",s);g_nred++;}}
static void done(void *u,const nvn_gpu_done *d){(void)u;(void)d;}
void rp_buf_reset(void);
static nvn_gpu_buf buffer(uint64_t bytes,const void *data){nvn_gpu_buf b=NULL;GPU(nvn_gpu_buf_create(bytes,NVN_GPU_MEM_SHARED,data,&b,&err));return b;}
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout){
 char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
 FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
 void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
 nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
 nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
// Shader/pass/pool registration are controlled fixtures. The mirror allocator,
// sync_write GPU exclusion, address resolver, draw implementation and Vulkan are real.
int g_s2a_logfd=2,s2a_nvn_rast_flip;
static alignas(4096) unsigned char guest[4096];
static struct rp_prog prog;static p_set_rt target;static tex_t target_tex;
static nvn_gpu_view target_view;static nvn_gpu_cmd active;
static uint64_t serial=1;static unsigned emitted;static int pass_open,expect_no_draw;
void __real_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a){REQUIRE(!expect_no_draw);emitted++;__real_nvn_gpu_draw(c,a);}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&prog;return 0;}
static int fixture_origin=1;
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return fixture_origin;}
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return 1;}
int rp_fence_done(uint64_t f){(void)f;return 1;}
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_flush(active);*last=serial;return active;}
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
int rp_post_gpu_write(uint64_t a,uint64_t n){(void)a;(void)n;return 0;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
// Deliberately untracked pool: every resolve resynchronizes CPU pages, so real
// rp_buf pending-GPU-write exclusion must protect the poisoned parameter bytes.
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return 1;}
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return 1;}
const void *nvn_dirty_span(const void *a,size_t n){(void)a;(void)n;return NULL;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;(void)g;(void)n;return -1;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)i;(void)c;if (t) memset(t, 0, sizeof *t);return NULL;}
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)i;(void)t;(void)k;(void)e;(void)c;if (o) memset(o, 0, sizeof *o);return NULL;}
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
// 此夹具未提供查询采样器证明，不使用内部查询对象。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }   // #1072：替身不判过滤能力
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int i,tex_t *t,view_t *v,int *h){(void)i;(void)t;(void)v;(void)h;return 0;}
uint64_t rp_tex_reg_gen(void){return 1;}uint64_t rp_tex_fact_sig(const uint64_t *h,int n,int b,int c){(void)h;(void)n;(void)b;(void)c;return 0;}
void rp_tex_mark_gpu_written(const tex_t *t){(void)t;}int rp_tex_debug_state(uint64_t s){(void)s;return 0;}
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s){(void)s;return NULL;}
const p_set_rt *rp_pass_target(void){return &target;}int rp_pass_slot_attached(int i){return i==0;}
void rp_pass_slot_size(int i,uint32_t *w,uint32_t *h){(void)i;*w=*h=64;}void rp_pass_noatt_size(uint32_t *w,uint32_t *h){(void)w;(void)h;}void rp_pass_note_draw(int d){(void)d;}
// #950 给 rp_draw.c 新加的通道依赖：中性替身（没有待折清除、不缩渲染区域）
int rp_pass_parked(void){return 0;}void rp_pass_prepare_draw(nvn_gpu_cmd c,uint32_t color_write_mask){(void)c;(void)color_write_mask;}void rp_pass_land_reads(nvn_gpu_cmd c,const tex_t *const *ts,int n){(void)c;(void)ts;(void)n;}
static void pass(nvn_gpu_cmd c,int clear){nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=target_view;p.color[0].load=clear?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD;p.color[0].store=NVN_GPU_STORE_STORE;p.color[0].clear.f[3]=1;p.w=p.h=64;p.layers=1;nvn_gpu_pass_begin(c,&p);pass_open=1;}
int rp_pass_begin(nvn_gpu_cmd c){if(!pass_open)pass(c,0);return 1;}void rp_pass_flush(nvn_gpu_cmd c){if(pass_open){nvn_gpu_pass_end(c);pass_open=0;}}
// ---- 注入：包 vkCreateGraphicsPipelines / vkCreateComputePipelines。inject_left = 1：真建成后报 VK_ERROR_UNKNOWN、句柄留在输出里
//   （模仿失败却没置空的驱动；那份对象归「驱动」，用例收尾自己销毁）；inject_left = 0：合规失败（真建的立刻销毁、输出置空）。
static int inject_n, inject_left; static VkPipeline left[16]; static int nleft, destroyed_left, bound_left;
uint64_t nvn_vk_pipe_fail_left_count(void) __attribute__((weak));
static VkPipeline inj(VkDevice d, VkResult r, VkPipeline *out) {
 if (!inject_n || r != VK_SUCCESS) return VK_NULL_HANDLE;
 inject_n--; VkPipeline p = out[0];
 if (inject_left) { if (nleft < 16) left[nleft++] = p; } else { extern void __real_vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*); __real_vkDestroyPipeline(d, p, NULL); out[0] = VK_NULL_HANDLE; }
 return p;
}
VkResult __real_vkCreateGraphicsPipelines(VkDevice,VkPipelineCache,uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*);
VkResult __wrap_vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *ci,const VkAllocationCallbacks *a,VkPipeline *out){
 VkResult r=__real_vkCreateGraphicsPipelines(d,c,n,ci,a,out); return inj(d,r,out) ? VK_ERROR_UNKNOWN : r; }
VkResult __real_vkCreateComputePipelines(VkDevice,VkPipelineCache,uint32_t,const VkComputePipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*);
VkResult __wrap_vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo *ci,const VkAllocationCallbacks *a,VkPipeline *out){
 VkResult r=__real_vkCreateComputePipelines(d,c,n,ci,a,out); return inj(d,r,out) ? VK_ERROR_UNKNOWN : r; }
static int is_left(VkPipeline p){for(int i=0;i<nleft;i++)if(left[i]==p)return 1;return 0;}
void __real_vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*);
void __wrap_vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a){if(p&&is_left(p))destroyed_left++;__real_vkDestroyPipeline(d,p,a);}
// 绑定检测：后端经两级回放的包装（nvn_vk_dq.inc）按 vkGetDeviceProcAddr 取到的指针调 vkCmdBindPipeline，--wrap 只截得到直接调用；
//   所以同时包 vkGetDeviceProcAddr，把 vkCmdBindPipeline 换成计数的这一层，两条路都记。
void __real_vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
static PFN_vkCmdBindPipeline real_bind;
static int binds_seen;   // 用例自检：绑定检测真的看得到绑定（每笔画出来的绘制都绑过管线）
static void bind_note(VkPipeline p){binds_seen++;if(p&&is_left(p))bound_left++;REQUIRE(p);}
void __wrap_vkCmdBindPipeline(VkCommandBuffer cb,VkPipelineBindPoint bp,VkPipeline p){bind_note(p);__real_vkCmdBindPipeline(cb,bp,p);}
static VKAPI_ATTR void VKAPI_CALL bind_via_ptr(VkCommandBuffer cb,VkPipelineBindPoint bp,VkPipeline p){bind_note(p);real_bind(cb,bp,p);}
PFN_vkVoidFunction __real_vkGetDeviceProcAddr(VkDevice,const char*);
PFN_vkVoidFunction __wrap_vkGetDeviceProcAddr(VkDevice d,const char *n){PFN_vkVoidFunction f=__real_vkGetDeviceProcAddr(d,n);if(f&&n&&!strcmp(n,"vkCmdBindPipeline")){real_bind=(PFN_vkCmdBindPipeline)f;return (PFN_vkVoidFunction)bind_via_ptr;}return f;}
static unsigned bad;
#define EXPECT(c,...) do{ if(!(c)){ bad++; fprintf(stderr,"【红】line %d: %s：",__LINE__,#c); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr);} }while(0)
// 一轮：k 笔同一配方的绘制（前 fails 笔的建管线被注入失败），最后一笔必须画出来
static void round_draws(struct s2a_vk_device *d,const char *dir,int left_mode,int fails,int draws,const char *what){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="pipe-fail";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);prog.color_out_mask=15;prog.sh[0]=shader(dir,"replay_front_face.vert",NVN_GPU_SH_VS,NULL);prog.sh[1]=shader(dir,"solid_green.frag",NVN_GPU_SH_FS,NULL);
 for(int i=0;i<32;i++)prog.identity[i]=(uint8_t)(0xa0+i);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 for(int i=0;i<8;i++){rp_S.skey[0][i]=(uint8_t)(0x10+i);rp_S.skey[1][i]=(uint8_t)(0x20+i);}rp_S.skey_ok=3;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;pass(active,1);rp_pass_flush(active);
 g_nred=0;nleft=0;destroyed_left=bound_left=0;binds_seen=0;inject_left=left_mode;inject_n=fails;
 const uint64_t left0=nvn_vk_pipe_fail_left_count?nvn_vk_pipe_fail_left_count():0;unsigned e0=emitted;
 p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;
 for(int i=0;i<draws;i++)rp_draw(active,&draw,NULL,&draw);
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 const uint8_t *out=nvn_gpu_buf_cpu(rb);unsigned green=0;for(unsigned j=0;j<4096;j++)if(out[j*4+1]==255&&!out[j*4])green++;
 const uint64_t leftn=(nvn_vk_pipe_fail_left_count?nvn_vk_pipe_fail_left_count():0)-left0;
 printf("pipe-fail %s：失败 %d 次后画出 %u 笔、绿 %u/4096；失败留下的句柄 %d 个，被销毁 %d 次、被绑定 %d 次；后端计「留了非空句柄」%llu 次；红行 %d 条\n",what,fails,emitted-e0,green,nleft,destroyed_left,bound_left,(unsigned long long)leftn,g_nred);
 for(int i=0;i<g_nred&&i<16;i++)printf("  红行 %d：%.300s\n",i,g_red[i]);
 EXPECT(emitted-e0==(unsigned)(draws-fails),"发出 %u 笔（期望 %d）",emitted-e0,draws-fails);EXPECT(green==4096,"绿 %u",green);
 EXPECT(destroyed_left==0,"失败留下的句柄被销毁 %d 次",destroyed_left);EXPECT(bound_left==0,"失败留下的句柄被绑定 %d 次",bound_left);
 EXPECT(binds_seen>0,"绑定检测没看到任何绑定（检测失效）");
 EXPECT(leftn==(uint64_t)(left_mode?fails:0),"后端计数 %llu",(unsigned long long)leftn);
 int want=0;for(int k=1;k<=fails;k++)if(!(k&(k-1)))want++;
 EXPECT(g_nred==want,"红行 %d 条（期望 %d：每配方第 1、2、4、8… 次）",g_nred,want);
 if(g_nred>0){const char *l=g_red[0];
  EXPECT(strstr(l,"图形管线建不出（这个配方第 1 次")!=NULL,"首行没有次数");EXPECT(strstr(l,"VkResult -13（UNKNOWN）")!=NULL,"首行没有 VkResult");
  EXPECT(strstr(l,left_mode?"输出句柄非空":"输出句柄为空")!=NULL,"首行没有输出句柄说明");
  EXPECT(strstr(l,"程序 a0a1a2a3a4a5a6a7")!=NULL,"首行没有程序身份");EXPECT(strstr(l,"顶点键 1011121314151617")&&strstr(l,"片元键 2021222324252627"),"首行没有阶段键");
  EXPECT(strstr(l,"配方：图元 4")&&strstr(l,"颜色格式 0:"),"首行没有配方摘要");}
 if(g_nred>3)EXPECT(strstr(g_red[3],"这个配方第 8 次")!=NULL,"第 4 行应是第 8 次");
 for(int i=0;i<nleft;i++)__real_vkDestroyPipeline(d->device,left[i],NULL);
 // 「驱动」自己的那份：用例收尾销毁（不然校验层报泄漏）
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
}
// 计算管线：后端同口径（VkResult、输出句柄不销毁）；回放侧红行与图形共用 pipe_fail_log
static void compute_fail(struct s2a_vk_device *d,const char *dir){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="pipe-fail-cs";GPU(nvn_gpu_open(&od,&err));
 nvn_gpu_layout_ent ent={0};ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_RW;ent.count=1;ent.stages=1u<<NVN_GPU_SH_CS;nvn_gpu_layout lay={0};lay.ent=&ent;lay.nent=1;
 nvn_gpu_shader cs=shader(dir,"count_args.comp",NVN_GPU_SH_CS,&lay);nvn_gpu_stage_desc sd={0};sd.sh=cs;nvn_gpu_pipe cp=NULL;
 nleft=0;destroyed_left=0;inject_left=1;inject_n=1;nvn_gpu_err e={0};
 nvn_gpu_rc r=nvn_gpu_cpipe_get(&sd,NVN_GPU_PIPE_SYNC,&cp,&e);
 printf("pipe-fail compute：rc=%d why=%s；留下 %d 个、被销毁 %d 次\n",r.code,e.why,nleft,destroyed_left);
 EXPECT(!nvn_gpu_ok(r)&&!cp,"计算管线应失败");EXPECT(strstr(e.why,"vkCreateComputePipelines 失败：VkResult -13（UNKNOWN），输出句柄非空")!=NULL,"why=%s",e.why);EXPECT(destroyed_left==0,"被销毁 %d 次",destroyed_left);
 r=nvn_gpu_cpipe_get(&sd,NVN_GPU_PIPE_SYNC,&cp,&e);EXPECT(nvn_gpu_ok(r)&&cp,"重试应建成");
 for(int i=0;i<nleft;i++)__real_vkDestroyPipeline(d->device,left[i],NULL);
 nvn_gpu_shader_release(cs);GPU(nvn_gpu_close(5000000000ull,&err));
}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 struct s2a_vk_device d;char why[256];if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 round_draws(&d,argv[1],1,1,2,"失败留非空句柄 1 次");
 round_draws(&d,argv[1],0,1,2,"合规失败 1 次");
 round_draws(&d,argv[1],1,9,10,"同配方连续失败 9 次（限频）");
 compute_fail(&d,argv[1]);
 REQUIRE(!s2a_vk_device_destroy(&d,1));
 printf("pipe-fail cases=4 failures%u\n",bad);return bad?1:0;
}
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
