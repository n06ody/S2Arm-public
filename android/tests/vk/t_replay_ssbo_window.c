// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 存储缓冲的绑定窗口：基址不对齐 minStorageBufferOffsetAlignment、长度超 maxStorageBufferRange——真实 rp_draw + rp_buf + Vulkan 后端，读回像素。
// 片元着色器读窗口里的第 1 个 uint（字节 4）：等同编译器在基址不对齐时生成的「地址 − (基址 & −对齐)」取数（这里基址余数固定是 4）。
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
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
#include <stdatomic.h>
#include <stdalign.h>
#include <time.h>
extern uint64_t nvn_vk_dbg_dq_recs(void);
static nvn_gpu_err err;
static atomic_int retired;
static unsigned failures;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){(void)n;return d;}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static void done(void *u,const nvn_gpu_done *d){(void)u;(void)d;}
static void retirement(void *u){(void)u;atomic_fetch_add(&retired,1);}
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
enum { GUEST_BYTES = 0x1000 + (128u << 20) + 0x20000, SB_OFF = 0x1000 };   // 顶点在前 4 KiB；存储缓冲从 4 KiB 起，能放下一块比 128 MiB 还长的
static alignas(4096) unsigned char guest[GUEST_BYTES];
static struct rp_prog prog;static p_set_rt target;static tex_t target_tex;
static nvn_gpu_view target_view;static nvn_gpu_cmd active;
static uint64_t serial=1;static unsigned emitted;static int pass_open,expect_no_draw;
void __real_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a){REQUIRE(!expect_no_draw);emitted++;__real_nvn_gpu_draw(c,a);}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&prog;return 0;}
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return 1;}
uint64_t rp_frame(void){return 1;}   // 固定帧号，与其他独立回放夹具一致
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
// 本夹具不模拟真实块代号，按未知回退逐页路径。
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)i;(void)c;if (t) memset(t, 0, sizeof *t);return NULL;}
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)i;(void)t;(void)k;(void)e;(void)c;if (o) memset(o, 0, sizeof *o);return NULL;}
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid,int tid){(void)tid;return rp_samp_by_id(sid);}
// 夹具不提供查询采样器；请求该能力时返回不可用。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f){return nvn_gpu_format_caps(f);}
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

// 一笔全屏绘制。addr_off：存储缓冲在池里的起点；size：游戏绑定的长度。期望像素 = 游戏地址 addr 处（窗口起点 + 余数）的那个 uint。
static int run_case(struct s2a_vk_device *d,const char *dir,uint64_t addr_off,uint64_t size,uint32_t *align,uint64_t *range,int *drew){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-replay-ssbo-window";GPU(nvn_gpu_open(&od,&err));
 nvn_gpu_caps caps;nvn_gpu_caps_get(&caps);*align=caps.min_ssbo_align;*range=caps.max_ssbo_range;
 memset(&prog,0,sizeof prog);nvn_gpu_layout fs_layout={0};
 prog.nent[1]=1;nvn_gpu_layout_ent *e=&prog.ent[1][0];e->set=0;e->binding=1;e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;e->src_reg=0;e->count=1;e->access=NVN_GPU_ACC_R;e->stages=1u<<NVN_GPU_SH_FS;
 fs_layout.ent=e;fs_layout.nent=1;
 prog.sh[0]=shader(dir,"quad.vert",NVN_GPU_SH_VS,NULL);prog.sh[1]=shader(dir,"replay_ssbo_window.frag",NVN_GPU_SH_FS,&fs_layout);prog.attr_mask=3;prog.color_out_mask=15;
 memset(guest,0,SB_OFF+0x100);
 float (*v)[8]=(float(*)[8])(void*)guest;const float xy[6][2]={{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
 for(unsigned j=0;j<6;j++){v[j][0]=xy[j][0];v[j][1]=xy[j][1];v[j][3]=v[j][7]=1;}
 const uint32_t want=0xff332211u;   // RGBA8 = (17,34,51,255)
 // 着色器始终读窗口字节 4；不对齐用例正好是 addr，已对齐用例则是 addr+4。
 const uint64_t sample_off=SB_OFF+4;
 memcpy(guest+sample_off,&want,4);
 const uint32_t poison=0xff00ff00u;memcpy(guest+sample_off+4,&poison,4);   // 错位 4 字节就读到这个
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.nattr=2;rp_S.nstream=1;rp_S.attr[0].fmt=rp_S.attr[1].fmt=NVN_FORMAT_RGBA32F;rp_S.attr[1].offset=16;rp_S.stream[0].stride=32;rp_S.vbuf[0].address=(uint64_t)(uintptr_t)guest;rp_S.vbuf[0].size=192;rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 rp_S.ssbo[1][0].addr=(uint64_t)(uintptr_t)(guest+addr_off);rp_S.ssbo[1][0].size=size;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;
 pass(active,1);rp_pass_flush(active);unsigned before=emitted;
 p_draw a={0};a.prim=4;a.count=6;a.instances=1;rp_draw(active,&a,NULL,&a);
 *drew=(int)(emitted-before);
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);
 nvn_gpu_rc rc=nvn_gpu_cmd_commit(active,1,done,NULL,&err);int ok=nvn_gpu_ok(rc);
 if(ok){GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
  const uint8_t *q=nvn_gpu_buf_cpu(rb);const uint8_t px[4]={17,34,51,255};unsigned bad=0;
  for(unsigned i=0;i<64*64;i++)if(memcmp(q+i*4,px,4))bad++;
  if(bad){fprintf(stderr,"FAIL off=%#llx size=%#llx: %u pixels differ, pixel 0 = %u,%u,%u,%u\n",(unsigned long long)addr_off,(unsigned long long)size,bad,q[0],q[1],q[2],q[3]);failures+=bad;}}
 else{fprintf(stderr,"FAIL off=%#llx size=%#llx: segment failed rc=%d why=%s\n",(unsigned long long)addr_off,(unsigned long long)size,err.rc.code,err.why);failures++;}
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);int t=atomic_load(&retired)+1;nvn_gpu_buf_release(rb,retirement,NULL);struct timespec pause={0,1000000};for(int i=0;i<5000&&atomic_load(&retired)<t;i++)nanosleep(&pause,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
 return ok?0:-1;
}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 struct s2a_vk_device d;char why[256];
 if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 printf("device: %s\n",d.properties.deviceName);
 uint32_t align=0;uint64_t range=0;int drew=0;
 // 1. 基址比对齐多 4 字节：窗口要从对齐处起、长度补 4
 run_case(&d,argv[1],SB_OFF+4,64,&align,&range,&drew);
 printf("minStorageBufferOffsetAlignment=%u maxStorageBufferRange=%llu\n",align,(unsigned long long)range);
 if(align<8){fprintf(stderr,"FAIL: device alignment %u leaves no room for a 4-byte remainder\n",align);return 2;}
 if(drew!=1||nvn_unimpl_count("draw","storage_buffer_offset_unaligned")){fprintf(stderr,"FAIL: unaligned base drew=%d\n",drew);failures++;}
 // 2. 已对齐、长度超范围上限：窗口夹到上限、具名记一次、照画
 const uint64_t big=range+0x10000;
 if(SB_OFF+big>GUEST_BYTES){fprintf(stderr,"FAIL: device range %llu too large for this fixture\n",(unsigned long long)range);return 2;}
 drew=0;run_case(&d,argv[1],SB_OFF,big,&align,&range,&drew);
 if(drew!=1||nvn_unimpl_count("draw","storage_buffer_window_clamped")!=1){fprintf(stderr,"FAIL: oversized storage buffer drew=%d clamped=%llu\n",drew,(unsigned long long)nvn_unimpl_count("draw","storage_buffer_window_clamped"));failures++;}
 REQUIRE(!s2a_vk_device_destroy(&d,1));
 printf("%s replay storage buffer window: align %u, bound %#llx > range %llu, failures=%u\n",failures?"FAIL":"PASS",align,(unsigned long long)big,(unsigned long long)range,failures);
 return failures?1:0;
}
