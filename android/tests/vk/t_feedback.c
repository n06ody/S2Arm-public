// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static nvn_gpu_err err;
static int dq_mode,opt_mode,copy_mode=1;
static unsigned failures;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"fixture failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#include "feedback_observer.h"
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){if(!strcmp(n,"nvk_dq"))return dq_mode;if(!strcmp(n,"nvk_opt_layout"))return opt_mode;if(!strcmp(n,"nvk_fb_copy"))return copy_mode;return d;}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static void done(void *u,const nvn_gpu_done *d){(void)u;REQUIRE(d->status==NVN_GPU_DONE_OK);}
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout) {
    char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
    FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
    void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
    nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
static void begin_pass(nvn_gpu_cmd c,nvn_gpu_view view,int load,unsigned size) {
    nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=view;p.color[0].load=load;
    p.color[0].store=NVN_GPU_STORE_STORE;p.color[0].clear.f[0]=.25f;p.color[0].clear.f[3]=1;
    p.w=p.h=size;p.layers=1;nvn_gpu_pass_begin(c,&p);
}
static void run_case(struct s2a_vk_device *device,const char *dir,int which) {
    observer_reset();nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-isolation";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent[2]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].access=NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    nvn_gpu_layout layout={0};layout.ent=ent;layout.nent=2;
    if(which==10){ent[0].count=2;ent[0].src_kind=NVN_RB_TEXP;observed_expect_samples=2;}
    if(which==11){ent[0].cls=NVN_GPU_RES_TEX_SAMP;layout.nent=1;}
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,which==10?"feedback_pool.frag":which==11?"feedback_combined.frag":"feedback.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex images[2]={0};nvn_gpu_view views[2]={0},sample=NULL;
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;
    td.d.depth=1;td.d.levels=which==4?2:1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    if(which==5||which==6){td.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;td.d.depth=2;}
    if(which==8)td.usage|=NVN_GPU_TEX_VIEW_FMT;
    view_t av={0};av.set_layers=1;av.nlayers=1;av.min_layer=which==5?1:0;av.set_target=1;av.target=NVN_TEXTURE_TARGET_2D;
    for(unsigned i=0;i<2;i++){GPU(nvn_gpu_tex_create(&td,&images[i],&err));GPU(nvn_gpu_view_create(images[i],&av,&views[i],&err));}
    view_t v={0};v.set_levels=1;v.nlevels=1;v.base_level=which==4?1:0;
    if(which==5||which==6){v=av;v.min_layer=1;}
    if(which==7){v.set_swizzle=1;v.swz[0]=3;v.swz[1]=2;v.swz[2]=4;v.swz[3]=5;}
    if(which==8){v.set_format=1;v.format=NVN_FORMAT_BGRA8;}
    GPU(nvn_gpu_view_create(images[which==0?1:0],&v,&sample,&err));
    samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));
    nvn_gpu_pool_table pool=NULL;
    if(which==10){nvn_gpu_pool_item item={.state=NVN_GPU_POOL_READY};item.resource.view=sample;nvn_gpu_pool_desc d={.kind=NVN_GPU_POOL_IMAGE,.count=1,.items=&item};d.dummy.view=sample;GPU(nvn_gpu_pool_create(&d,&pool,&err));}
    unsigned char sentinel[80];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(sizeof sentinel,NVN_GPU_MEM_SHARED,sentinel,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    for(unsigned i=0;i<2;i++){begin_pass(cmd,views[i],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_pass_end(cmd);}
    if(which==4){begin_pass(cmd,sample,NVN_GPU_LOAD_CLEAR,2);nvn_gpu_pass_end(cmd);}
    if(which==6){begin_pass(cmd,sample,NVN_GPU_LOAD_CLEAR,4);nvn_gpu_pass_end(cmd);}
    begin_pass(cmd,views[0],which==2?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD,4);
    nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;
    if(which==10){nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&b[1],1);GPU(nvn_gpu_pool_bind(cmd,NVN_GPU_SH_FS,0,pool,&err));}
    else if(which==11){b[0].cls=NVN_GPU_RES_TEX_SAMP;b[0].u.ts.view=sample;b[0].u.ts.samp=sampler;nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,1);}
    else nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,2);
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;unsigned draws=which==3||which==9||which==10?2:1;
    for(unsigned i=0;i<draws;i++){
        if(which==9&&i==1){nvn_gpu_pass_end(cmd);begin_pass(cmd,views[1],NVN_GPU_LOAD_LOAD,4);} // same bind: must restore original, now changed by draw 1
        nvn_gpu_draw(cmd,&a);
    }
    nvn_gpu_pass_end(cmd);nvn_gpu_copy_tex_buf(cmd,images[which==9?1:0],0,which==5?1:0,NULL,rb,0,16,64);
    // Release every source owner before submit; command refs and private snapshots must survive.
    if(pool)nvn_gpu_pool_release(pool);
    nvn_gpu_samp_release(sampler);nvn_gpu_view_release(sample);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    if(which==14){
        observed_aborted=1;nvn_gpu_cmd_abort(cmd); // no GPU work; queued DQ recording still must finish before destruction
        REQUIRE(observed_draws==1);REQUIRE(nvn_gpu_fence_done()==0&&nvn_gpu_fence_failed()==0);
        failures+=observed_overlap+(observed_copies!=1);
        printf("feedback abort dq=%d opt=%d draws=%u live-overlap=%u copies=%u\n",dq_mode,opt_mode,observed_draws,observed_overlap,observed_copies);
        nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));return;
    }
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
    REQUIRE(observed_draws==draws);unsigned pixels_bad=0;const unsigned char *bytes=nvn_gpu_buf_cpu(rb);
    unsigned red=which==7||which==8?64:draws==2?192:128;
    for(unsigned i=0;i<16;i++)if(bytes[i*4]!=red||bytes[i*4+1]!=(which==7?64:0)||bytes[i*4+2]!=(which==8?64:0)||bytes[i*4+3]!=255)pixels_bad++;
    for(unsigned i=64;i<80;i++)if(bytes[i]!=0xa5)pixels_bad++;
    int control=which==0||which==4||which==6;unsigned copy_bad=observed_copies!=(control?0:which==9?1:draws);
    printf("feedback dq=%d opt=%d case=%d draws=%u live-overlap=%u copies=%u copy-bad=%u pixel-bad=%u\n",dq_mode,opt_mode,which,draws,observed_overlap,observed_copies,copy_bad,pixels_bad);
    failures+=observed_overlap+copy_bad+pixels_bad;
    nvn_gpu_buf_release(rb,NULL,NULL);
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));
    REQUIRE(observed_early_destroy==0);
}
static void run_depth(struct s2a_vk_device *device,const char *dir,unsigned format,int which) {
    observer_reset();nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-depth";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent[2]={{0}};
    ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].count=1;ent[i].access=NVN_GPU_ACC_R;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    nvn_gpu_layout layout={.ent=ent,.nent=2};
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,"feedback_depth.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;pd.cfmt[0]=NVN_FORMAT_RGBA8;pd.dfmt=format;
    pd.ds.dt=pd.ds.dw=1;pd.ds.df=8;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex images[2]={0};nvn_gpu_view views[2]={0},sample=NULL;
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    for(unsigned i=0;i<2;i++){td.d.format=i?format:NVN_FORMAT_RGBA8;GPU(nvn_gpu_tex_create(&td,&images[i],&err));GPU(nvn_gpu_view_create(images[i],NULL,&views[i],&err));}
    GPU(nvn_gpu_view_create(images[1],NULL,&sample,&err));
    samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));
    unsigned char init[80];memset(init,0xa5,sizeof init);nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(sizeof init,NVN_GPU_MEM_SHARED,init,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=views[0];p.color[0].load=NVN_GPU_LOAD_CLEAR;p.color[0].store=NVN_GPU_STORE_STORE;
    p.depth.view=views[1];p.depth.load=NVN_GPU_LOAD_CLEAR;p.depth.store=NVN_GPU_STORE_STORE;p.clear_depth=.25f;p.w=p.h=4;p.layers=1;
    nvn_gpu_pass_begin(cmd,&p);nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,2);
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;
    nvn_gpu_draw(cmd,&a);nvn_gpu_draw(cmd,&a);nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd,images[0],0,0,NULL,rb,0,16,64);
    nvn_gpu_samp_release(sampler);nvn_gpu_view_release(sample);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
    REQUIRE(observed_draws==2);unsigned bad=0;const unsigned char *bytes=nvn_gpu_buf_cpu(rb);
    // D32 stores 0.25 -> 0.5 exactly; second output is 0.75, rounded to UNORM 191.
    for(unsigned i=0;i<16;i++)if(bytes[i*4]!=191||bytes[i*4+1]||bytes[i*4+2]||bytes[i*4+3]!=255)bad++;
    for(unsigned i=64;i<80;i++)if(bytes[i]!=0xa5)bad++;
    unsigned copy_bad=observed_copies!=2;
    printf("feedback dq=%d opt=%d case=%d draws=2 live-overlap=%u copies=%u copy-bad=%u pixel-bad=%u\n",dq_mode,opt_mode,which,observed_overlap,observed_copies,copy_bad,bad);
    failures+=observed_overlap+copy_bad+bad;nvn_gpu_buf_release(rb,NULL,NULL);
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(observed_early_destroy==0);
}
int main(int argc,char **argv) {
    if(argc!=5)return 2;
    dq_mode=atoi(argv[2]);opt_mode=atoi(argv[3]);REQUIRE((dq_mode==0||dq_mode==1)&&(opt_mode==0||opt_mode==1));
    copy_mode=atoi(argv[4]);REQUIRE(copy_mode==0||copy_mode==1);
    struct s2a_vk_device device;char why[256];
    if(s2a_vk_device_create(&device,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
    for(int c=0;c<12;c++)run_case(&device,argv[1],c);
    run_depth(&device,argv[1],NVN_FORMAT_DEPTH32F,12);
    run_depth(&device,argv[1],NVN_FORMAT_DEPTH32F_STENCIL8,13);
    run_case(&device,argv[1],14);
    REQUIRE(!s2a_vk_device_destroy(&device,1));printf("feedback cases=15 failures=%u\n",failures);return failures?1:0;
}
