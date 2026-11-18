// SPDX-License-Identifier: Apache-2.0
// #977 零属性：真 rp_draw + 真 Vulkan 后端。顶点着色器读 location 3，游戏没给它设格式；
// 期望核心补一条步长 0、内容全 0 的 RGBA32F 属性，着色器读到 (0,0,0,0) 画红，校验层 0 条。
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
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return 1;}
uint64_t rp_frame(void){return 1;}   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身，同 t_replay_indirect）
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
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid){(void)tid;return rp_samp_by_id(sid);}
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
// mode 0：只设了 attr0（nattr=1），location 3 在属性表外；mode 1：nattr=4，attr3 的格式是 0、流号指着已用的流 0。
static void run_case(struct s2a_vk_device *d,const char *dir,int mode){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-replay-zero-attr";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);
 prog.sh[0]=shader(dir,"zattr.vert",NVN_GPU_SH_VS,NULL);prog.sh[1]=shader(dir,"quad.frag",NVN_GPU_SH_FS,NULL);prog.attr_mask=1u|1u<<3;prog.color_out_mask=15;
 memset(guest,0,sizeof guest);float (*v)[4]=(float(*)[4])(void*)(guest+256);
 static const float corner[6][2]={{-1,-1},{-1,1},{1,-1},{1,-1},{-1,1},{1,1}};
 for(unsigned j=0;j<6;j++){v[j][0]=corner[j][0];v[j][1]=corner[j][1];v[j][2]=0;v[j][3]=1;}
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.nattr=mode?4:1;rp_S.nstream=1;rp_S.attr[0].fmt=NVN_FORMAT_RGBA32F;rp_S.stream[0].stride=16;
 rp_S.vbuf[0].address=(uint64_t)(uintptr_t)(guest+256);rp_S.vbuf[0].size=96;rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;
 pass(active,1);unsigned before=emitted;
 p_draw a={0};a.prim=4;a.count=6;a.instances=1;rp_draw(active,&a,NULL,&a);
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 const uint8_t *q=nvn_gpu_buf_cpu(rb);unsigned bad=0;for(unsigned i=0;i<4096;i++)if(q[i*4]!=255||q[i*4+1]||q[i*4+2]||q[i*4+3]!=255)bad++;
 failures+=bad;
 printf("zero-attr mode=%d：画了 %u 笔，未接 %u 种，首像素 %u,%u,%u,%u，不符像素 %u（期望 location 3 读到 0 → 全红）\n",mode,emitted-before,nvn_unimpl_kinds(),q[0],q[1],q[2],q[3],bad);
 REQUIRE(emitted-before==1);REQUIRE(nvn_unimpl_kinds()==0);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);int t=atomic_load(&retired)+1;nvn_gpu_buf_release(rb,retirement,NULL);struct timespec pause={0,1000000};for(int i=0;i<5000&&atomic_load(&retired)<t;i++)nanosleep(&pause,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
}
int main(int argc,char **argv){if(argc!=2)return 2;struct s2a_vk_device d;char why[256];if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 for(int mode=0;mode<2;mode++)run_case(&d,argv[1],mode);
 REQUIRE(!s2a_vk_device_destroy(&d,1));printf("real rp_draw zero-attr cases2 pixel-failures%u\n",failures);return failures?1:0;}

// 本夹具不模拟真实块代号；返回未知，让回放沿用逐页保守路径。
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
