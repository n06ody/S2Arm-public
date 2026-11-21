// SPDX-License-Identifier: Apache-2.0
// Independent production rp_draw/rp_buf GPU test; never includes legacy tests.
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "rp_state.h"
#include "rp_res.h"
#include "no_libpack_fixture.h"
#include "nvn_unimpl.h"
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
uint64_t nvn_vk_nvnbar_state(void);
void nvn_vk_nvnbar_submit(uint64_t now);
static nvn_gpu_err err;
static atomic_int retired;
static unsigned failures;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
static int hz_flag=2;
static int depth_ab_seconds;
int nvn_plat_flag(const char *n,int d){if(!strcmp(n,"nvk_opt_layout"))return !!getenv("S2A_REPLAY_INDIRECT_OPT_LAYOUT");if(!strcmp(n,"nvk_binst_hoist"))return hz_flag;if(!strcmp(n,"nvk_nvnbar_ab"))return depth_ab_seconds;return !strcmp(n,"nvk_dq")?!!getenv("S2A_REPLAY_INDIRECT_DQ"):d;}
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
static int fixture_origin=1;
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return fixture_origin;}
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return 1;}
int rp_fence_done(uint64_t f){(void)f;return 1;}
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_flush(active);*last=serial;rp_gpu_wr_gen++;return active;}   // 同 rp_exec.c：同步落主段 = 主段里写了镜像
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
void rp_seg_prog_note(const struct rp_prog *p) { (void)p; }   // rp_exec.c 的段失败键记录：本用例不验
int rp_post_gpu_write(uint64_t a,uint64_t n){(void)a;(void)n;return 0;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
// Deliberately untracked pool: every resolve resynchronizes CPU pages, so real
// rp_buf pending-GPU-write exclusion must protect the poisoned parameter bytes.
// run_hoist 用受跟踪的一页（g_track）：页代号 g_gen，写跟踪钟 g_now；改游戏内存时两个一起推，下一次解析就按真实路径重同步。
static int g_track;static uint64_t g_gen=1,g_now=1;
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return g_track?g_now:1;}
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return !g_track;}
const void *nvn_dirty_span(const void *a,size_t n){(void)a;(void)n;return g_track?(const void *)guest:NULL;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;if(!g_track)return -1;for(size_t i=0;i<n;i++)g[i]=g_gen;return (long)n;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)i;(void)c;if (t) memset(t, 0, sizeof *t);return NULL;}
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)i;(void)t;(void)k;(void)e;(void)c;if (o) memset(o, 0, sizeof *o);return NULL;}
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
// 此夹具未提供查询采样器证明，不使用内部查询对象。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }   // #1072：替身不判过滤能力
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }   // 同 rp_samp_for_tex：替身不给默认采样器
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
static int cut_count_on;static unsigned cuts;   // run_hoist：绘制序列里开着的通道被收了几次
int rp_pass_begin(nvn_gpu_cmd c){if(!pass_open)pass(c,0);return 1;}void rp_pass_flush(nvn_gpu_cmd c){if(pass_open){nvn_gpu_pass_end(c);pass_open=0;if(cut_count_on)cuts++;}}
int rp_pass_is_open(void){return pass_open;}
static void pixels(const uint8_t *p,int drew){for(unsigned y=2;y<62;y++)for(unsigned r=0;r<4;r++)for(unsigned x=2;x<14;x++){const uint8_t *v=p+(y*64+r*16+x)*4;int red=drew&&r==0;if(v[0]!=(red?255:0)||v[1]||v[2]||v[3]!=255)failures++;}}
static void run_case(struct s2a_vk_device *d,const char *dir,int index,int mdi){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-replay-indirect";GPU(nvn_gpu_open(&od,&err));
 const int neg=mdi>=10;const char *reason=NULL;
 const int drv=mdi==3;memset(&prog,0,sizeof prog);nvn_gpu_layout graphics_layout[2]={{0}};
 if(drv)for(int st=0;st<2;st++){prog.nent[st]=1;nvn_gpu_layout_ent *e=&prog.ent[st][0];e->set=0;e->binding=(uint16_t)st;e->cls=NVN_GPU_RES_UBO;e->src_kind=NVN_RB_DRV0;e->count=1;e->access=NVN_GPU_ACC_R;e->stages=1u<<(st?NVN_GPU_SH_FS:NVN_GPU_SH_VS);graphics_layout[st].ent=e;graphics_layout[st].nent=1;}
 prog.sh[0]=shader(dir,drv?"replay_drv0.vert":"quad.vert",NVN_GPU_SH_VS,drv?&graphics_layout[0]:NULL);prog.sh[1]=shader(dir,drv?"replay_drv0.frag":"quad.frag",NVN_GPU_SH_FS,drv?&graphics_layout[1]:NULL);prog.attr_mask=drv?1:3;prog.color_out_mask=15;
 nvn_gpu_layout_ent ent={0};ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_RW;ent.count=1;ent.stages=1u<<NVN_GPU_SH_CS;
 nvn_gpu_layout lay={0};lay.ent=&ent;lay.nent=1;lay.push_bytes=drv?8:0;nvn_gpu_shader cs=shader(dir,drv?"replay_drv0_args.comp":"count_args.comp",NVN_GPU_SH_CS,&lay);nvn_gpu_stage_desc sd={0};sd.sh=cs;nvn_gpu_pipe cp=NULL;GPU(nvn_gpu_cpipe_get(&sd,NVN_GPU_PIPE_SYNC,&cp,&err));nvn_gpu_shader_release(cs);
 memset(guest,0xa5,sizeof guest);uint32_t *args=(uint32_t*)(void*)guest;args[0]=1;args[4]=0;
 for(unsigned i=0;i<5;i++){unsigned b=8+i*8;args[b]=6;args[b+1]=1;args[b+2]=18;args[b+3]=args[b+4]=0;}
 float (*v)[8]=(float(*)[8])(void*)(guest+256);const unsigned corners[6]={0,1,2,2,1,3};
 for(unsigned r=0;r<4;r++)for(unsigned j=0;j<6;j++){unsigned k=corners[j];float *w=v[r*6+j];memset(w,0,32);w[0]=-1+r*.5f+((k&2)?.5f:0);w[1]=(k&1)?1:-1;w[3]=w[7]=1;w[4+(r<3?r:2)]=1;}
 for(unsigned i=0;i<24;i++){if(index==1)((uint16_t*)(void*)(guest+2052))[i]=(uint16_t)i;else ((uint32_t*)(void*)(guest+2052))[i]=i;}
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.nattr=2;rp_S.nstream=1;rp_S.attr[0].fmt=rp_S.attr[1].fmt=NVN_FORMAT_RGBA32F;rp_S.attr[1].offset=16;rp_S.stream[0].stride=32;rp_S.vbuf[0].address=(uint64_t)(uintptr_t)(guest+256);rp_S.vbuf[0].size=768;rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;nvn_gpu_buf mirror=NULL;uint64_t off=0;REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,192,&mirror,&off));uint64_t key=0,id=rp_buf_gpu_write_note((uint64_t)(uintptr_t)guest,192,&key);REQUIRE(id);
 nvn_gpu_binding ssbo={0};ssbo.cls=NVN_GPU_RES_SSBO;ssbo.u.b.buf=mirror;ssbo.u.b.off=off;ssbo.u.b.len=192;nvn_gpu_set_pipe(active,cp);nvn_gpu_bind(active,NVN_GPU_SH_CS,&ssbo,1);uint32_t pc[2]={3,index!=0};if(drv)nvn_gpu_push(active,NVN_GPU_SH_CS,0,pc,8);if(!getenv("S2A_REPLAY_INDIRECT_SKIP_PRODUCER"))nvn_gpu_dispatch(active,cp,1,1,1,NULL,0);nvn_gpu_barrier(active,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);
 pass(active,1);rp_pass_flush(active);unsigned before=emitted;
 for(int round=0;round<(drv?3:1);round++){
  if(drv){rp_R.has_sc=1;rp_R.sc.x=round==0?0:round==1?21:42;rp_R.sc.y=0;rp_R.sc.w=round==2?22:21;rp_R.sc.h=64;}
  if(drv&&round==1){rp_pass_flush(active);pc[0]=7;nvn_gpu_set_pipe(active,cp);nvn_gpu_bind(active,NVN_GPU_SH_CS,&ssbo,1);nvn_gpu_push(active,NVN_GPU_SH_CS,0,pc,8);if(!getenv("S2A_REPLAY_INDIRECT_SKIP_PRODUCER"))nvn_gpu_dispatch(active,cp,1,1,1,NULL,0);nvn_gpu_barrier(active,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);rp_dirty_all();}
  int direct=drv&&round==2;
  if(direct){
   unsigned saved=emitted;expect_no_draw=1;
   if(index){p_draw_idx bad={0};bad.prim=4;bad.itype=index;bad.ibuf=(uint64_t)(uintptr_t)(guest+2052);bad.ind=(uint64_t)(uintptr_t)(guest+4096);rp_draw(active,NULL,&bad,&bad);}
   else{p_draw bad={0};bad.prim=4;bad.ind=(uint64_t)(uintptr_t)(guest+4096);rp_draw(active,&bad,NULL,&bad);}
   expect_no_draw=0;REQUIRE(emitted==saved);REQUIRE(nvn_unimpl_count("draw","indirect_buffer_unresolved")==1);
  }

  uint64_t badind=0,badidx=0;int badtype=index,badprim=4;
  if(neg){
   reason="indirect_buffer_unresolved";
   if(mdi==10)badind=(uint64_t)(uintptr_t)(guest+4096);
   if(mdi==11)badind=(uint64_t)(uintptr_t)(guest+4084);
   if(mdi==12)badind=(uint64_t)(uintptr_t)(guest+33);
   if(mdi==13||mdi==14){badtype=mdi==13?-1:3;reason="index_type_invalid";}
   if(mdi==15){badtype=0;reason="indirect_topology_or_index_conversion";}
   if(mdi>=16&&mdi<=19){const int ps[]={2,7,8,9};badprim=ps[mdi-16];reason="indirect_topology_or_index_conversion";}
   if(mdi>=20){reason="index_buffer_unresolved";badidx=(uint64_t)(uintptr_t)(guest+(mdi==20?4096:mdi==21?4095:2053));}
   expect_no_draw=1;
  }

  if(index){p_draw_idx a={0};a.prim=4;a.itype=index;a.ibuf=(uint64_t)(uintptr_t)(guest+2052);a.ind=direct?0:(uint64_t)(uintptr_t)(guest+32);a.count=direct?6:0;a.instances=direct?1:0;a.base_vertex=direct?0:INT32_MAX;a.base_instance=direct?0:INT32_MAX;if(mdi==2||mdi==4){a.mdi_cnt=(uint64_t)(uintptr_t)(guest+16);a.mdi_i=mdi==4?2:0;}if(neg){a.prim=badprim;a.itype=badtype;if(badind)a.ind=badind;if(badidx)a.ibuf=badidx;}rp_draw(active,NULL,&a,&a);}
  else{p_draw a={0};a.prim=4;a.ind=direct?0:(uint64_t)(uintptr_t)(guest+32);a.count=direct?6:0;a.first=direct?0:INT32_MAX;a.instances=direct?1:0;a.base_instance=direct?0:INT32_MAX;if(mdi==2||mdi==4){a.mdi_cnt=(uint64_t)(uintptr_t)(guest+16);a.mdi_i=mdi==4?2:0;}if(neg){a.prim=badprim;if(badind)a.ind=badind;}rp_draw(active,&a,NULL,&a);}
  if(neg){expect_no_draw=0;REQUIRE(emitted==before);REQUIRE(nvn_unimpl_count("draw",reason)==1);REQUIRE(nvn_unimpl_count("buf","address_outside_pools")== (unsigned)(mdi==10||mdi==11));REQUIRE(nvn_unimpl_kinds()==((mdi==10||mdi==11)?2:1));}
 }
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 if(drv){const uint8_t *out=nvn_gpu_buf_cpu(rb);const uint8_t expected[3][4]={{255,0,255,255},{0,255,0,255},{255,255,255,255}};for(unsigned y=2;y<62;y++)for(unsigned x=2;x<62;x++){if(x==20||x==21||x==41||x==42)continue;unsigned k=x<21?0:x<42?1:2;if(memcmp(out+(y*64+x)*4,expected[k],4))failures++;}}
 else if(neg){const uint8_t *q=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++)if(q[i*4]||q[i*4+1]||q[i*4+2]||q[i*4+3]!=255)failures++;}
 else pixels(nvn_gpu_buf_cpu(rb),!mdi);
 REQUIRE(emitted-before==(unsigned)(drv?3:!mdi));
 if(neg)printf("boundary index=%d mode=%d reason=%s no-draw clear-unchanged\n",index,mdi,reason);
 // 带计数的记录由命令循环按已解码记录成组（#658，rp_exec.c count_feed → rp_draw_count_batch），rp_draw 不往后读命令流：
 //   mdi=2：组首（序号 0）直接进 rp_draw、没有成组的计数 → indirect_count_group_unavailable；
 //   mdi=4：组中记录（序号 2）单独进 rp_draw = 组没成 → indirect_count_group_broken。两种都不画、不报另一个名字。
 if(mdi==2){REQUIRE(nvn_unimpl_count("draw","indirect_count_group_unavailable")==1);REQUIRE(nvn_unimpl_count("draw","indirect_count_group_broken")==0);}
 if(mdi==4){REQUIRE(nvn_unimpl_count("draw","indirect_count_group_broken")==1);REQUIRE(nvn_unimpl_count("draw","indirect_count_group_unavailable")==0);}rp_buf_gpu_write_land(key,id,0);rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);int t=atomic_load(&retired)+1;nvn_gpu_buf_release(rb,retirement,NULL);struct timespec pause={0,1000000};for(int i=0;i<5000&&atomic_load(&retired)<t;i++)nanosleep(&pause,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
}
// Empty valid storage descriptors must still execute the actual draw. Read both
// ends of Ryujinx's 64 KiB fallback; padding in the readback stays untouched.
static void run_null_storage(struct s2a_vk_device *d,const char *dir) {
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-replay-null-storage";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);prog.nent[0]=1;prog.color_out_mask=15;
 nvn_gpu_layout_ent *e=&prog.ent[0][0];e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;e->access=NVN_GPU_ACC_R;e->count=1;e->stages=1u<<NVN_GPU_SH_VS;
 nvn_gpu_layout layout={0};layout.ent=e;layout.nent=1;
 prog.sh[0]=shader(dir,"replay_null_storage.vert",NVN_GPU_SH_VS,&layout);prog.sh[1]=shader(dir,"quad.frag",NVN_GPU_SH_FS,NULL);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));
 uint8_t sentinel[16416];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf rb=buffer(sizeof sentinel,sentinel);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;pass(active,1);rp_pass_flush(active);
 unsigned before=emitted;p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;rp_draw(active,&draw,NULL,&draw);
 REQUIRE(emitted==before+1);REQUIRE(!nvn_unimpl_kinds());rp_pass_flush(active);
 nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 const uint8_t *out=nvn_gpu_buf_cpu(rb);for(unsigned j=0;j<4096;j++)REQUIRE(out[j*4]==0&&out[j*4+1]==255&&out[j*4+2]==0&&out[j*4+3]==255);
 for(unsigned j=16384;j<sizeof sentinel;j++)REQUIRE(out[j]==0xa5);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
 puts("null-storage: valid unbound slot draws 4096 green pixels; 64 KiB endpoints zero; 32 sentinel bytes preserved");
}
#define CHECK REQUIRE
#include "depth_blit_test.h"
// Warm replay state is restored after the depth/stencil helper overwrites physical GPU state.
static void run_depth_state_restore(struct s2a_vk_device *d,const char *dir) {
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-depth-state-restore";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);prog.color_out_mask=15;prog.nent[1]=1;
 nvn_gpu_layout_ent *entry=&prog.ent[1][0];entry->set=0;entry->binding=0;entry->cls=NVN_GPU_RES_UBO;entry->src_kind=NVN_RB_CBUF;entry->src_reg=0;entry->count=1;entry->access=NVN_GPU_ACC_R;entry->stages=1u<<NVN_GPU_SH_FS;
 nvn_gpu_layout layout={0};layout.ent=entry;layout.nent=1;
 prog.sh[0]=shader(dir,"depth_state.vert",NVN_GPU_SH_VS,NULL);
 nvn_gpu_shader_src fs={0};fs.stage=NVN_GPU_SH_FS;fs.kind=NVN_GPU_BLOB_SPIRV;fs.entry="main";fs.layout=&layout;fs.blob=depth_state_frag_spv;fs.bytes=sizeof depth_state_frag_spv;GPU(nvn_gpu_shader_load(&fs,&prog.sh[1],&err));
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));
 uint8_t sentinel[16416];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf rb=buffer(sizeof sentinel,sentinel);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 float green[4]={0,1,0,1};memcpy(guest+256,green,sizeof green);rp_S.ubo[NVN_GPU_SH_FS][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ubo[NVN_GPU_SH_FS][0].size=sizeof green;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;pass(active,1);rp_pass_flush(active);
 unsigned before=emitted;p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;rp_draw(active,&draw,NULL,&draw);
 REQUIRE(emitted==before+1);REQUIRE(!nvn_unimpl_kinds());rp_pass_flush(active);
 // Warm graphics state must survive the helper's pipelines/descriptors/viewport.
 // Clear red first: a missing or partial second green draw cannot hide behind the first.
 nvn_gpu_pass_desc red={0};red.w=red.h=64;red.layers=1;red.ncolor=1;red.color[0].view=target_view;red.color[0].load=NVN_GPU_LOAD_CLEAR;red.color[0].store=NVN_GPU_STORE_STORE;red.color[0].clear.f[0]=red.color[0].clear.f[3]=1;
 nvn_gpu_pass_begin(active,&red);nvn_gpu_pass_end(active);
 nvn_gpu_tex_desc dsdesc={0};dsdesc.d.target=NVN_TEXTURE_TARGET_2D;dsdesc.d.format=NVN_FORMAT_DEPTH24_STENCIL8;dsdesc.d.width=dsdesc.d.height=4;dsdesc.d.depth=dsdesc.d.levels=dsdesc.d.samples=1;dsdesc.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
 nvn_gpu_tex dsimage[2]={0};nvn_gpu_view dsview[2]={0};
 for(unsigned i=0;i<2;i++){GPU(nvn_gpu_tex_create(&dsdesc,&dsimage[i],&err));GPU(nvn_gpu_view_create(dsimage[i],NULL,&dsview[i],&err));nvn_gpu_pass_desc fill={0};fill.w=fill.h=4;fill.layers=1;fill.depth.view=fill.stencil.view=dsview[i];fill.depth.load=fill.stencil.load=NVN_GPU_LOAD_CLEAR;fill.depth.store=fill.stencil.store=NVN_GPU_STORE_STORE;fill.clear_depth=.25f;fill.clear_stencil=0xa5;nvn_gpu_pass_begin(active,&fill);nvn_gpu_pass_end(active);}
 NVNcopyRegion source={0,0,0,4,4,1},destination={1,1,0,2,2,1};nvn_gpu_blit(active,dsview[0],&source,dsview[1],&destination,0);
 rp_draw(active,&draw,NULL,&draw);REQUIRE(emitted==before+2);rp_pass_flush(active);
 for(unsigned i=0;i<2;i++){nvn_gpu_view_release(dsview[i]);nvn_gpu_tex_release(dsimage[i]);}
 nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 const uint8_t *out=nvn_gpu_buf_cpu(rb);for(unsigned j=0;j<4096;j++)REQUIRE(out[j*4]==0&&out[j*4+1]==255&&out[j*4+2]==0&&out[j*4+3]==255);
 for(unsigned j=16384;j<sizeof sentinel;j++)REQUIRE(out[j]==0xa5);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
 puts("depth-state: 4096 green pixels after helper; 32 sentinel bytes preserved");
}
static void run_depth_copy(struct s2a_vk_device *d, unsigned ab_seconds) {
 depth_ab_seconds=(int)ab_seconds;
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-depth-copy";GPU(nvn_gpu_open(&od,&err));
 test_depth_blit(ab_seconds);GPU(nvn_gpu_close(5000000000ull,&err));
 depth_ab_seconds=0;
}
// Independent oracle: StateUpdater flips API winding when queue flip is off,
// EnumConversion inverts GAL for Vulkan, and C80 defines the viewport sign.
// For this NDC-CCW triangle, the table says which raw NVN state must draw.
static void run_front_origin(struct s2a_vk_device *d,const char *dir) {
 static const unsigned visible[2][2][2]={{{0,1},{1,0}},{{1,0},{0,1}}};
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="NVN-front-origin-matrix";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);prog.color_out_mask=15;
 prog.sh[0]=shader(dir,"replay_front_face.vert",NVN_GPU_SH_VS,NULL);prog.sh[1]=shader(dir,"solid_green.frag",NVN_GPU_SH_FS,NULL);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;rp_S.poly.cull=NVN_FACE_BACK;
 unsigned wrong=0,cases=0;
 for(unsigned api=0;api<2;api++)for(unsigned flip=0;flip<2;flip++)for(unsigned ll=0;ll<2;ll++){
  if(rp_S.poly.front!=api){rp_S.poly.front=api;rp_pso_dirty=1;}s2a_nvn_rast_flip=(int)flip;fixture_origin=(int)ll;
  GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;pass(active,1);rp_pass_flush(active);
  p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;rp_draw(active,&draw,NULL,&draw);rp_pass_flush(active);REQUIRE(!nvn_unimpl_kinds());
  nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1000+cases,done,NULL,&err));GPU(nvn_gpu_fence_wait(1000+cases,5000000000ull,&err));
  const uint8_t *out=nvn_gpu_buf_cpu(rb);unsigned bad=0;for(unsigned i=0;i<4096;i++)if(out[4*i]||out[4*i+1]!=(visible[api][flip][ll]?255:0)||out[4*i+2]||out[4*i+3]!=255)bad++;
  printf("front-origin api=%u queue-flip=%u origin-ll=%u expected=%s wrong-pixels=%u\n",api,flip,ll,visible[api][flip][ll]?"green":"culled",bad);if(bad)wrong++;cases++;
 }
 fixture_origin=1;s2a_nvn_rast_flip=0;rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
 printf("front-origin matrix cases=%u failed=%u\n",cases,wrong);REQUIRE(!wrong);
}
// 通道级批量拷 base instance（rp_draw.c，debug.s2a.nvk_binst_hoist）：一个通道里 4 笔带索引的间接绘制，记录 i 的 firstInstance = 11 (i+1)，
// 第 i 笔画第 i 列（16 像素宽的裁剪），顶点 / 片元两阶段都从各自驱动段 c0 + 0x644 读 base instance 写进红 / 绿。真 rp_draw / rp_buf / 后端。
//   mode 0：批量拷，期望 0 次为拷贝收通道，四列 11/22/33/44；
//   mode 1：第 1、2 笔之间计算派发把记录 2 的 firstInstance 改成 99（命令流里有 OP_DISPATCH）：前瞻停在派发前，第 2 列 99；
//   mode 2：第 1、2 笔之间游戏内存里改记录 2（CPU，页代号前进）：第 2 笔解析时同步落主段，批量表作废，第 2 列 99，收通道 1 次（同步本身的）；
//   mode 3：开关 0（逐笔拷）：四列同 mode 0，为拷贝收通道 3 次。
//   mode 0–2 用开关 2（一次多区域拷贝 + 只到顶点 / 片元常量读的屏障，同步校验核屏障够不够）；mode 4：开关 1（每段一次拷贝各带全屏障），同 mode 0。
static uint8_t *hz_put(uint8_t *at,uint32_t op,uint32_t payload){op_hdr h={op,(uint32_t)((8+payload+7)&~7u)};memcpy(at,&h,8);memset(at+8,0,h.size-8);return at;}
static void run_hoist(struct s2a_vk_device *d,const char *dir,int mode){
 nvn_gpu_open_desc od={0};od.host_device=d;od.app_label="real-replay-binst-hoist";GPU(nvn_gpu_open(&od,&err));
 memset(&prog,0,sizeof prog);nvn_gpu_layout graphics_layout[2]={{0}};
 for(int st=0;st<2;st++){prog.nent[st]=1;nvn_gpu_layout_ent *e=&prog.ent[st][0];e->set=0;e->binding=(uint16_t)st;e->cls=NVN_GPU_RES_UBO;e->src_kind=NVN_RB_DRV0;e->count=1;e->access=NVN_GPU_ACC_R;e->stages=1u<<(st?NVN_GPU_SH_FS:NVN_GPU_SH_VS);graphics_layout[st].ent=e;graphics_layout[st].nent=1;}
 prog.sh[0]=shader(dir,"replay_binst.vert",NVN_GPU_SH_VS,&graphics_layout[0]);prog.sh[1]=shader(dir,"replay_binst.frag",NVN_GPU_SH_FS,&graphics_layout[1]);prog.attr_mask=1;prog.color_out_mask=15;
 nvn_gpu_layout_ent ent={0};ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_RW;ent.count=1;ent.stages=1u<<NVN_GPU_SH_CS;
 nvn_gpu_layout lay={0};lay.ent=&ent;lay.nent=1;lay.push_bytes=8;nvn_gpu_shader cs=shader(dir,"replay_binst_args.comp",NVN_GPU_SH_CS,&lay);nvn_gpu_stage_desc sd={0};sd.sh=cs;nvn_gpu_pipe cp=NULL;GPU(nvn_gpu_cpipe_get(&sd,NVN_GPU_PIPE_SYNC,&cp,&err));nvn_gpu_shader_release(cs);
 memset(guest,0,sizeof guest);
 for(unsigned i=0;i<4;i++){uint32_t *r=(uint32_t*)(void*)(guest+32+20*i);r[0]=6;r[1]=1;r[2]=0;r[3]=0;r[4]=11*(i+1);}
 static const float quad[6][2]={{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
 for(unsigned j=0;j<6;j++){float *w=(float*)(void*)(guest+256+32*j);w[0]=quad[j][0];w[1]=quad[j][1];w[2]=0;w[3]=1;}
 for(unsigned i=0;i<6;i++)((uint32_t*)(void*)(guest+2052))[i]=i;
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));nvn_gpu_buf rb=buffer(16384,NULL);
 memset(&target_tex,0,sizeof target_tex);target_tex.d=td.d;target_tex.serial=1;memset(&target,0,sizeof target);target.n=1;target.color[0]=&target_tex;
 hz_flag=mode==3?0:mode==4?1:2;g_track=1;g_gen=g_now=1;
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();rp_S.nattr=1;rp_S.nstream=1;rp_S.attr[0].fmt=NVN_FORMAT_RGBA32F;rp_S.stream[0].stride=32;rp_S.vbuf[0].address=(uint64_t)(uintptr_t)(guest+256);rp_S.vbuf[0].size=192;rp_S.color.logic_op=3;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;
 nvn_gpu_buf mirror=NULL;uint64_t off=0,key=0,id=0;REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,192,&mirror,&off));
 if(mode==1){id=rp_buf_gpu_write_note((uint64_t)(uintptr_t)guest,192,&key);REQUIRE(id);}   // 派发改的是镜像：登记成本提交的 GPU 写，同步不盖它
 pass(active,1);rp_pass_flush(active);
 // 命令流：4 条 OP_DRAW_IDX（mode 1 在第 1、2 笔之间插 OP_DISPATCH），OP_END
 static alignas(8) uint8_t cmd[1024];uint8_t *at=cmd,*rec[4];
 for(unsigned i=0;i<4;i++){
  if(mode==1&&i==2){uint8_t *h=hz_put(at,OP_DISPATCH,sizeof(p_dispatch));at+=((op_hdr*)(void*)h)->size;}
  rec[i]=hz_put(at,OP_DRAW_IDX,sizeof(p_draw_idx));p_draw_idx *a=(p_draw_idx*)(void*)(rec[i]+8);a->prim=4;a->itype=2;a->ibuf=(uint64_t)(uintptr_t)(guest+2052);a->ind=(uint64_t)(uintptr_t)(guest+32+20*i);at+=((op_hdr*)(void*)rec[i])->size;
 }
 hz_put(at,OP_END,0);
 unsigned before=emitted;cuts=0;cut_count_on=1;
 for(unsigned i=0;i<4;i++){
  if(i==2&&mode==1){cut_count_on=0;rp_pass_flush(active);uint32_t pc[2]={(32+20*2+16)/4,99};nvn_gpu_binding ssbo={0};ssbo.cls=NVN_GPU_RES_SSBO;ssbo.u.b.buf=mirror;ssbo.u.b.off=off;ssbo.u.b.len=192;nvn_gpu_set_pipe(active,cp);nvn_gpu_bind(active,NVN_GPU_SH_CS,&ssbo,1);nvn_gpu_push(active,NVN_GPU_SH_CS,0,pc,8);nvn_gpu_dispatch(active,cp,1,1,1,NULL,0);nvn_gpu_barrier(active,NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);rp_dirty_all();cut_count_on=1;}
  if(i==2&&mode==2){((uint32_t*)(void*)(guest+32+20*2))[4]=99;g_gen=++g_now;}
  rp_R.has_sc=1;rp_R.sc.x=(int)(16*i);rp_R.sc.y=0;rp_R.sc.w=16;rp_R.sc.h=64;
  rp_draw_stream=rec[i];rp_draw(active,NULL,(const p_draw_idx*)(const void*)(rec[i]+8),rec[i]);rp_draw_stream=NULL;
 }
 cut_count_on=0;
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 const uint8_t *out=nvn_gpu_buf_cpu(rb);unsigned bad=0;uint8_t seen[4][2]={{0}};
 for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){unsigned c=x/16;uint8_t want=(uint8_t)((mode==1||mode==2)&&c==2?99:11*(c+1));const uint8_t *v=out+(y*64+x)*4;if(y==32&&x%16==8){seen[c][0]=v[0];seen[c][1]=v[1];}if(v[0]!=want||v[1]!=want||v[2]||v[3]!=255)bad++;}
 const unsigned want_cuts=mode==3?3:mode==2?1:0;
 printf("binst-hoist mode=%d flag=%d draws=%u cuts=%u(expect %u) columns vs/fs=%u/%u %u/%u %u/%u %u/%u wrong-pixels=%u unimpl=%u\n",mode,hz_flag,emitted-before,cuts,want_cuts,seen[0][0],seen[0][1],seen[1][0],seen[1][1],seen[2][0],seen[2][1],seen[3][0],seen[3][1],bad,nvn_unimpl_kinds());
 if(bad||cuts!=want_cuts||emitted-before!=4||nvn_unimpl_kinds())failures++;
 if(id)rp_buf_gpu_write_land(key,id,0);
 g_track=0;hz_flag=2;rp_R.has_sc=0;rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);int t=atomic_load(&retired)+1;nvn_gpu_buf_release(rb,retirement,NULL);struct timespec pause={0,1000000};for(int i=0;i<5000&&atomic_load(&retired)<t;i++)nanosleep(&pause,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
}
#include "mirror_blit_test.h"
int main(int argc,char **argv) {
 int depth_only=argc==3&&!strcmp(argv[2],"--depth-only");
 int hoist_only=(argc==3&&!strcmp(argv[2],"--hoist-only"))||(argc==2&&getenv("S2A_REPLAY_INDIRECT_HOIST_ONLY"));   // 只跑通道级批量拷五例（变异负控用）
 int mirror_only=argc==3&&!strcmp(argv[2],"--mirror-only");
 if(argc!=2&&!depth_only&&!mirror_only&&!hoist_only)return 2;
 struct s2a_vk_device d;char why[256];
 if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 if(hoist_only){for(int m=0;m<5;m++)run_hoist(&d,argv[1],m);REQUIRE(!s2a_vk_device_destroy(&d,1));printf("binst-hoist only: pixel-failures%u\n",failures);return failures?1:0;}
 if(mirror_only){run_color_mirror(&d);REQUIRE(!s2a_vk_device_destroy(&d,1));return 0;}
 if(!depth_only){
  for(int m=0;m<5;m++)run_hoist(&d,argv[1],m);   // 通道级批量拷（rp_draw.c nvk_binst_hoist）
  for(int index=0;index<3;index++){run_case(&d,argv[1],index,0);run_case(&d,argv[1],index,2);run_case(&d,argv[1],index,4);run_case(&d,argv[1],index,3);}
  for(int index=0;index<3;index++)for(int mode=10;mode<=22;mode++){if(!index&&((mode>=13&&mode<=15)||mode>=20))continue;run_case(&d,argv[1],index,mode);}
  run_null_storage(&d,argv[1]);run_null_storage(&d,argv[1]);
  run_depth_state_restore(&d,argv[1]);
  run_front_origin(&d,argv[1]);
 }
 run_depth_copy(&d,0);
 const char *probe=getenv("S2A_TEST_NVK_NVNBAR");
 if(probe&&!strcmp(probe,"1"))run_depth_copy(&d,30);
 if(!depth_only)run_color_mirror(&d);
 uint64_t dq=nvn_vk_dbg_dq_recs();REQUIRE(getenv("S2A_REPLAY_INDIRECT_DQ")?dq>0:dq==0);
 printf("DQ records=%llu\n",(unsigned long long)dq);REQUIRE(!s2a_vk_device_destroy(&d,1));
 if(depth_only)puts("real depth-copy only: 4 format/subresource cases passed");
 else printf("real rp_draw/rp_buf ordinary-indirect+DRV0 cases12 plus-boundaries33 pixel-failures%u\n",failures);
 return failures?1:0;
}

// 本夹具不模拟真实块代号；返回未知，让回放沿用逐页保守路径。
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
