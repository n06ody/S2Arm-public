// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 渲染分辨率倍率（片 1）：真 rp_pass / rp_draw / rp_res / rp_buf + 真 Vulkan 后端（lavapipe，同步校验），两级录制两档。
// 片元着色器把 FragCoord ÷ render_scale[0] 写成颜色（红 = 游戏 x、绿 = 游戏 y），读回存储逐像素核：
//   ① 可缩目标 200x100（倍率 2 → 存储 400x200；0.5 → 100x50；1 → 不变）：存储尺寸、视口（缺省 = 整张）、裁剪（游戏 (10,5) 100x50，起点向下 / 终点向上取整）、
//      render_scale[0] 三者合起来，存储像素 (x,y) 在裁剪内 = (⌊(x+0.5)/s⌋, ⌊(y+0.5)/s⌋)、裁剪外 = 清屏蓝；
//   ② 随后一个没声明按倍率处理的用途（直接取视图）：存储缩回 200x100，读回逐像素 = 游戏坐标（倍率 2 时两像素同值，线性缩小精确）；
//   ③ 方形 128x128 在倍率 2 不可缩：存储不变、像素 = 游戏坐标。
//   ④ 同一通道挂深度附件（r3 真机 9000 倍率 0.75 整局黑屏）：D24S8、只有深度的 D32F 后端深度 blit 都收 → 两张都缩；
//      D16 资格不放行（精度，同 iOS）→ 该深度按 1×、整组附件按 1× 开、段照常提交、像素 = 1×。
//      后端不收某种深度时（负控：depth_blit 去掉 D32F）→ 换存储前的询问兜住：该深度 1×、整组 1×、会话继续（尺寸红、不会提交失败）。
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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
extern uint64_t nvn_vk_dbg_dq_recs(void);
static nvn_gpu_err err;
static unsigned failures, cases;
static int dq_mode;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){return !strcmp(n,"nvk_dq")?dq_mode:d;}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
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
int g_s2a_logfd=2,s2a_nvn_rast_flip;
static alignas(4096) unsigned char guest[4096];
static struct rp_prog prog;static nvn_gpu_cmd active;
static uint64_t serial=1;static unsigned emitted;static int completed;
void __real_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a){emitted++;__real_nvn_gpu_draw(c,a);}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&prog;return 0;}
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return 0;}
uint64_t rp_frame(void) { return 1; }
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return 1;}
int rp_fence_done(uint64_t f){return !f || completed;}
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_end(active);*last=serial;return active;}   // 同 rp_exec.c：落主段只收通道（rp_pass_end），不落地挂起清除——调用方持着 rp_res 的锁
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
int rp_occl_want(void){return 0;}void rp_occl_pass_opened(int d){REQUIRE(!d);}
void rp_occl_pass_closed(void){}void rp_occl_pause(nvn_gpu_cmd c){(void)c;}
int rp_post_gpu_write(uint64_t a,uint64_t n){(void)a;(void)n;return 0;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return 1;}
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return 1;}
const void *nvn_dirty_span(const void *a,size_t n){(void)a;(void)n;return NULL;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;(void)g;(void)n;return -1;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
int nvn_dirty_since(const void *a,size_t n,uint64_t s){(void)a;(void)n;(void)s;return 0;}
long nvn_dirty_written_pages(const void *sp,const void *a,size_t l,uint64_t st,uint64_t *b,size_t nb){(void)sp;(void)a;(void)l;(void)st;(void)b;(void)nb;return -1;}
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
int nvn_dirty_block_snapshot(const void *s, const void *a, size_t n, uint64_t *v, uint64_t *e) { (void)s; (void)a; (void)n; (void)v; (void)e; return 0; }

static void clear_blue(void){p_clear_color cc={0};cc.index=0;cc.mask=15;cc.rgba[2]=1;cc.rgba[3]=1;rp_pass_clear_color(active,&cc,&cc);}
// 读回 t 现在的存储（第 0 级），逐像素按 expect 核；返回错像素数
static unsigned check(const tex_t *t,float s_store,int scissored,uint32_t *ow,uint32_t *oh){
 uint32_t w=0,h=0;rp_rs_level(t,0,&w,&h);*ow=w;*oh=h;
 nvn_gpu_buf rb=buffer((uint64_t)w*h*4,NULL);nvn_gpu_tex g=rp_tex_backend_by_serial(t->serial);REQUIRE(g);
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;completed=0;
 nvn_gpu_copy_tex_buf(active,g,0,0,NULL,rb,0,(uint64_t)w*4,(uint64_t)w*h*4);
 GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));completed=1;
 const uint8_t *p=nvn_gpu_buf_cpu(rb);unsigned bad=0;
 // 裁剪（游戏 (10,5) 100x50）换到存储坐标：起点向下、终点向上取整
 const double sx0=floor(10*s_store),sy0=floor(5*s_store),sx1=ceil(110*s_store),sy1=ceil(55*s_store);
 for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
  const uint8_t *v=p+((size_t)y*w+x)*4;uint8_t e[4];
  const int in=!scissored||(x>=sx0&&x<sx1&&y>=sy0&&y<sy1);
  if(in){e[0]=(uint8_t)((uint32_t)((x+0.5)/s_store)&255u);e[1]=(uint8_t)((uint32_t)((y+0.5)/s_store)&255u);e[2]=0;e[3]=255;}
  else{e[0]=0;e[1]=0;e[2]=255;e[3]=255;}
  if(memcmp(v,e,4))bad++;
 }
 nvn_gpu_buf_release(rb,NULL,NULL);return bad;
}
static void run_case(struct s2a_vk_device *device,const char *dir,const char *scale,int square,int depth_fmt){
 nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="replay-res-scale";GPU(nvn_gpu_open(&od,&err));
 rp_rs_configure(scale);rp_res_reset();rp_pass_reset();rp_draw_reset();rp_state_reset();nvn_unimpl_reset();rp_buf_reset();
 const float s=rp_rs_setting();
 memset(&prog,0,sizeof prog);prog.color_out_mask=0xff;prog.support_bytes=4096;
 nvn_gpu_layout_ent *e=&prog.ent[1][0];prog.nent[1]=1;e->set=0;e->binding=0;e->cls=NVN_GPU_RES_UBO;e->src_kind=NVN_RB_SUPPORT;e->count=1;e->access=NVN_GPU_ACC_R;e->stages=1u<<NVN_GPU_SH_FS;
 nvn_gpu_layout layout={0};layout.ent=e;layout.nent=1;layout.support_bytes=4096;
 prog.sh[0]=shader(dir,"depth_state.vert",NVN_GPU_SH_VS,NULL);prog.sh[1]=shader(dir,"res_scale_coord.frag",NVN_GPU_SH_FS,&layout);
 tex_t t;memset(&t,0,sizeof t);t.d.target=NVN_TEXTURE_TARGET_2D;t.d.format=NVN_FORMAT_RGBA8;t.d.width=square?128:200;t.d.height=square?128:100;t.d.depth=t.d.levels=1;
 t.d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE;t.serial=1000+(uint64_t)cases;
 tex_t dt=t;dt.d.format=depth_fmt;dt.serial=2000+(uint64_t)cases;   // 深度附件：同尺寸、同标志
 rp_S.color.logic_op=3;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;rp_S.chmask.mask=0xff;
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;completed=0;
 p_set_rt target={0};target.n=1;target.color[0]=&t;target.depth=depth_fmt?&dt:NULL;rp_pass_set_rt(active,&target);
 clear_blue();
 rp_R.has_sc=1;rp_R.sc.x=10;rp_R.sc.y=5;rp_R.sc.w=100;rp_R.sc.h=50;
 unsigned before=emitted;p_draw dr={0};dr.prim=4;dr.count=3;dr.instances=1;rp_draw(active,&dr,NULL,&dr);
 rp_pass_flush(active);GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));completed=1;
 REQUIRE(emitted-before==1);REQUIRE(!nvn_unimpl_kinds());
 const float want=square||depth_fmt==NVN_FORMAT_DEPTH16?1:s;uint32_t w=0,h=0;
 unsigned bad1=check(&t,want,1,&w,&h);
 int size1=w==(uint32_t)ceil(t.d.width*want)&&h==(uint32_t)ceil(t.d.height*want);
 uint32_t dw=0,dh=0;
 if(depth_fmt){rp_rs_level(&dt,0,&dw,&dh);size1=size1&&dw==(uint32_t)ceil(dt.d.width*want)&&dh==(uint32_t)ceil(dt.d.height*want);}
 // ② 没声明按倍率处理的用途：直接按（贴图，视图）取视图（拷贝端点同路）→ 存储缩回游戏尺寸
 GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;completed=0;
 REQUIRE(rp_view_for(&t,NULL,active));rp_pass_flush(active);
 GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));completed=1;
 uint32_t w2=0,h2=0;unsigned bad2=0;
 if(want>=1)bad2=check(&t,1,1,&w2,&h2);   // 倍率 ≥ 1：缩回后与 1× 逐像素相同（2× 时线性缩小取两像素同值）
 else{rp_rs_level(&t,0,&w2,&h2);}          // 倍率 < 1：放回 1× 是插值，只核尺寸
 const int size2=w2==t.d.width&&h2==t.d.height;
 printf("res-scale dq=%d scale=%s %s%s：存储 %ux%u（期望 %ux%u）深度存储 %ux%u 错像素 %u；未声明用途后存储 %ux%u 错像素 %u\n",dq_mode,scale?scale:"1",square?"方形 128":"200x100",
        depth_fmt==NVN_FORMAT_DEPTH32F?" + D32F 深度":depth_fmt==NVN_FORMAT_DEPTH24_STENCIL8?" + D24S8 深度":depth_fmt==NVN_FORMAT_DEPTH16?" + D16 深度":"",w,h,(uint32_t)ceil(t.d.width*want),(uint32_t)ceil(t.d.height*want),dw,dh,bad1,w2,h2,bad2);
 cases++;failures+=bad1+bad2+!size1+!size2;
 rp_res_reset();rp_pass_reset();rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);GPU(nvn_gpu_close(5000000000ull,&err));
}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 dq_mode=atoi(argv[2]);REQUIRE(dq_mode==0||dq_mode==1);
 struct s2a_vk_device device;char why[256];
 if(s2a_vk_device_create(&device,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
 run_case(&device,argv[1],NULL,0,0);run_case(&device,argv[1],"2",0,0);run_case(&device,argv[1],"0.5",0,0);run_case(&device,argv[1],"0.75",0,0);run_case(&device,argv[1],"2",1,0);
 run_case(&device,argv[1],"0.75",0,NVN_FORMAT_DEPTH24_STENCIL8);run_case(&device,argv[1],"2",0,NVN_FORMAT_DEPTH24_STENCIL8);
 run_case(&device,argv[1],"0.75",0,NVN_FORMAT_DEPTH32F);run_case(&device,argv[1],"2",0,NVN_FORMAT_DEPTH32F);run_case(&device,argv[1],"0.75",0,NVN_FORMAT_DEPTH16);
 rp_rs_configure(NULL);
 REQUIRE(!s2a_vk_device_destroy(&device,1));
 printf("res-scale cases=%u failures=%u\n",cases,failures);
 return failures?1:0;
}
