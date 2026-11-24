// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef S2A_NVK_POOL_HOST_TEST
extern uint64_t nvn_gpu_vk_test_pool_sets(int which);   // 整池组读数（只增不清零）：5 取到键相同的回收组 6 取到键不同的 7 回收新建 8 没走回收 9 反馈补丁写入项数
extern uint64_t nvn_gpu_vk_test_prs(int which);          // 回收：0 现存组 1 其中空闲
#endif
static nvn_gpu_err err;
static int dq_mode,opt_mode,copy_mode=1,pool_fb_mode=1;   // pool_fb_mode：nvk_pool_fb（1 = 整池数组也为写着的附件拷快照，旧行为；0 = 缺省，不拷）
static unsigned failures,begins,ncases;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"fixture failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#include "feedback_observer.h"
static void on_begin(const VkRenderPassBeginInfo *p){(void)p;begins++;}   // 物理渲染通道的开始次数（反馈重开会多一次）
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
static int diag_pass, diag_snapshot;
int nvn_plat_flag(const char *n,int d){
    if(!strcmp(n,"nvk_gpu_passes"))return diag_pass;
    if(!strcmp(n,"nvk_fb_snapshots"))return diag_snapshot;
    if(!strcmp(n,"nvk_dq"))return dq_mode;
    if(!strcmp(n,"nvk_opt_layout"))return opt_mode;
    if(!strcmp(n,"nvk_fb_copy"))return copy_mode;
    if(!strcmp(n,"nvk_pool_fb"))return pool_fb_mode;
    return d;
}
static unsigned skip_logs;   // 「整池数组免拷快照」逐条日志的条数（限流：每个（管线、图）首次一行；不含「已记满」那一句）
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");if(s&&strstr(s,"整池数组免拷快照（"))__atomic_fetch_add(&skip_logs,1u,__ATOMIC_RELAXED);}
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
    observer_reset();begins=0;skip_logs=0;ncases++;nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-isolation";GPU(nvn_gpu_open(&od,&err));
    printf("feedback begin dq=%d opt=%d pool_fb=%d case=%d\n",dq_mode,opt_mode,pool_fb_mode,which);   // 校验层的报错夹在两个 begin 之间，脚本按它归到案例（白名单按案例放行）
    nvn_gpu_layout_ent ent[2]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].access=NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    nvn_gpu_layout layout={0};layout.ent=ent;layout.nent=2;
    if(which==19)layout.push_bytes=4;   // 案例 19 的着色器用一个 uint 推送常量当整池数组的运行时下标
    if(which==10||which==16||which==17||which==18){ent[0].count=2;ent[0].src_kind=NVN_RB_TEXP;observed_expect_samples=2;}
    if(which==15||which==19){ent[0].count=3;ent[0].src_kind=NVN_RB_TEXP;observed_expect_samples=3;}   // 两项 + 表尾占位
    if(which==11){ent[0].cls=NVN_GPU_RES_TEX_SAMP;layout.nent=1;}
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,which==10||which==16||which==17||which==18?"feedback_pool.frag":which==19?"feedback_pool_dyn.frag":which==15?"feedback_pool_phantom.frag":which==11?"feedback_combined.frag":"feedback.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;
    pd.chmask.mask=which==16||which==17?240:15;   // 案例 16 / 17：0 号附件的写掩码为 0 = 本笔不写它（池里登记的恰是这张图）
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
    nvn_gpu_pool_table pool=NULL;nvn_gpu_view other=NULL;
    if(which==10||which==16||which==18){nvn_gpu_pool_item item={.state=NVN_GPU_POOL_READY};item.resource.view=sample;nvn_gpu_pool_desc d={.kind=NVN_GPU_POOL_IMAGE,.count=1,.items=&item};d.dummy.view=sample;GPU(nvn_gpu_pool_create(&d,&pool,&err));}
    if(which==17){   // 同一张图的两个不同视图（槽 0 = sample，表尾占位 = other），都与本笔没写的 0 号附件重叠：每笔两个都要拷快照（按图索引的串链不能漏掉第二个）
        GPU(nvn_gpu_view_create(images[0],&v,&other,&err));
        nvn_gpu_pool_item item={.state=NVN_GPU_POOL_READY};item.resource.view=sample;nvn_gpu_pool_desc d={.kind=NVN_GPU_POOL_IMAGE,.count=1,.items=&item};d.dummy.view=other;GPU(nvn_gpu_pool_create(&d,&pool,&err));
    }
    if(which==15||which==19){   // 槽 0 = 别的图，槽 1 = 本通道正在写的那张图，表尾占位 = 槽 0 的图。15：着色器只采样槽 0（槽 1 只登记、没人读）；19：着色器按推送常量下标采样槽 1（真读写着的附件）
        GPU(nvn_gpu_view_create(images[1],&v,&other,&err));
        nvn_gpu_pool_item items[2]={{.state=NVN_GPU_POOL_READY},{.state=NVN_GPU_POOL_READY}};items[0].resource.view=other;items[1].resource.view=sample;
        nvn_gpu_pool_desc d={.kind=NVN_GPU_POOL_IMAGE,.count=2,.items=items};d.dummy.view=other;GPU(nvn_gpu_pool_create(&d,&pool,&err));
    }
    unsigned char sentinel[80];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(sizeof sentinel,NVN_GPU_MEM_SHARED,sentinel,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    for(unsigned i=0;i<2;i++){begin_pass(cmd,views[i],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_pass_end(cmd);}
    if(which==4){begin_pass(cmd,sample,NVN_GPU_LOAD_CLEAR,2);nvn_gpu_pass_end(cmd);}
    if(which==6){begin_pass(cmd,sample,NVN_GPU_LOAD_CLEAR,4);nvn_gpu_pass_end(cmd);}
    begin_pass(cmd,views[0],which==2?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD,4);
    nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;
    if(which==10||which==15||which==16||which==17||which==18||which==19){nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&b[1],1);GPU(nvn_gpu_pool_bind(cmd,NVN_GPU_SH_FS,0,pool,&err));}
    else if(which==11){b[0].cls=NVN_GPU_RES_TEX_SAMP;b[0].u.ts.view=sample;b[0].u.ts.samp=sampler;nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,1);}
    else nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,2);
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;unsigned draws=which==3||which==9||which==10||which==15||which==16||which==17||which==18||which==19?2:1;
    if(which==19){const unsigned idx=1;nvn_gpu_push(cmd,NVN_GPU_SH_FS,0,&idx,sizeof idx);}   // 运行时下标 = 1：整池数组里本通道正在写的那张附件
    for(unsigned i=0;i<draws;i++){
        if(which==9&&i==1){nvn_gpu_pass_end(cmd);begin_pass(cmd,views[1],NVN_GPU_LOAD_LOAD,4);} // same bind: must restore original, now changed by draw 1
        nvn_gpu_draw(cmd,&a);
    }
    nvn_gpu_pass_end(cmd);nvn_gpu_copy_tex_buf(cmd,images[which==9?1:0],0,which==5?1:0,NULL,rb,0,16,64);
    // Release every source owner before submit; command refs and private snapshots must survive.
    if(pool)nvn_gpu_pool_release(pool);
    if(other)nvn_gpu_view_release(other);
    nvn_gpu_samp_release(sampler);nvn_gpu_view_release(sample);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    if(which==14){
        observed_aborted=1;nvn_gpu_cmd_abort(cmd); // no GPU work; queued DQ recording still must finish before destruction
        REQUIRE(observed_draws==1);REQUIRE(nvn_gpu_fence_done()==0&&nvn_gpu_fence_failed()==0);
        failures+=observed_overlap+(observed_copies!=1);
        printf("feedback abort dq=%d opt=%d draws=%u live-overlap=%u copies=%u\n",dq_mode,opt_mode,observed_draws,observed_overlap,observed_copies);
        nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));return;
    }
    if(which==18){   // 免拷守卫探针：只录制不提交（中止）。断言「免拷只在 GENERAL 档（opt=0）且没开旧行为开关（pool_fb=0）时发生」——
                     // opt=1 / pool_fb=1 必须照旧拷快照、拆通道。守卫被撤掉时这里按读数变红，不靠驱动崩溃才发现，也不把无效布局提交给驱动。
        observed_aborted=1;nvn_gpu_cmd_abort(cmd);
        REQUIRE(observed_draws==draws);REQUIRE(nvn_gpu_fence_done()==0&&nvn_gpu_fence_failed()==0);
        const int skip18=!pool_fb_mode&&!opt_mode;
        unsigned copy_bad=observed_copies!=(skip18?0:draws),overlap_bad=observed_overlap!=(skip18?4u:0u),begins_bad=skip18?begins!=3:begins<=3,logs_bad=skip_logs!=(skip18?1u:0u);
        printf("feedback guard dq=%d opt=%d pool_fb=%d case=%d draws=%u live-overlap=%u copies=%u copy-bad=%u begins=%u begins-bad=%u overlap-bad=%u skip-logs=%u logs-bad=%u\n",dq_mode,opt_mode,pool_fb_mode,which,draws,observed_overlap,observed_copies,copy_bad,begins,begins_bad,overlap_bad,skip_logs,logs_bad);
        failures+=copy_bad+overlap_bad+begins_bad+logs_bad;
        nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));return;
    }
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
    REQUIRE(nvn_gpu_fence_failed()==0);   // 时间线没进失败终态（done 回调里另有「状态 OK」断言：没有粘滞错误）
    REQUIRE(observed_draws==draws);unsigned pixels_bad=0;const unsigned char *bytes=nvn_gpu_buf_cpu(rb);
    // 整池数组里登记着本通道正在写的附件：缺省（nvk_pool_fb=0）不拷快照、不拆通道——着色器读不读它由表里的索引决定，
    // 整池「可能被采样」不等于「这一笔会读」（对齐 Ryujinx：数组段不做反馈环追踪）。opt 模式、nvk_pool_fb=1 仍走旧行为。
    // 案例 10（常量下标读）、19（推送常量下标读）：着色器真去读本通道正在写的附件，值未定义，缺省档只断言「不崩、不进失败终态、无粘滞错误、越界哨兵不动」。
    const int skip=(which==10||which==15||which==19)&&!pool_fb_mode&&!opt_mode;
    unsigned red=which==7||which==8||which==16||which==17?64:which==15?128:draws==2?192:128;   // 16 / 17：本笔不写附件，清屏值 0.25 原样留着
    if(!(skip&&(which==10||which==19)))
        for(unsigned i=0;i<16;i++)if(bytes[i*4]!=red||bytes[i*4+1]!=(which==7?64:0)||bytes[i*4+2]!=(which==8?64:0)||bytes[i*4+3]!=255)pixels_bad++;
    for(unsigned i=64;i<80;i++)if(bytes[i]!=0xa5)pixels_bad++;
    int control=which==0||which==4||which==6;unsigned copy_bad=observed_copies!=(skip?0:control?0:which==9?1:which==17?2*draws:draws);   // 17：每笔两个视图各拷一份
    unsigned overlap_want=skip?(which==10?4:2):0,overlap_bad=observed_overlap!=overlap_want;   // 缺省下描述符里仍是原图：10 的两个槽都是附件，15 / 19 只有槽 1 是
    unsigned begins_bad=0;if(which==10||which==15||which==19)begins_bad=skip?begins!=3:begins<=3;   // 缺省：2 次清屏 + 1 次主通道，没有反馈重开；旧行为每笔多重开一次
    if(which==16||which==17)begins_bad=begins!=5;   // 池里登记的是「本笔没写」的附件：缺省与旧行为一样照旧拷快照、拆通道（只免「写着的」）
    // 免拷逐条日志限流：每个（管线、图）首次一行——两笔 draw 只记一行；没免拷（旧行为 / opt=1 / 别的案例）一行都不记
    unsigned logs_bad=skip_logs!=(skip?1u:0u);
    printf("feedback dq=%d opt=%d pool_fb=%d case=%d draws=%u live-overlap=%u copies=%u copy-bad=%u begins=%u begins-bad=%u overlap-bad=%u pixel-bad=%u skip-logs=%u logs-bad=%u\n",dq_mode,opt_mode,pool_fb_mode,which,draws,observed_overlap,observed_copies,copy_bad,begins,begins_bad,overlap_bad,pixels_bad,skip_logs,logs_bad);
    failures+=overlap_bad+copy_bad+begins_bad+pixels_bad+logs_bad;
    nvn_gpu_buf_release(rb,NULL,NULL);
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));
    REQUIRE(observed_early_destroy==0);
}
static void run_depth(struct s2a_vk_device *device,const char *dir,unsigned format,int which) {
    observer_reset();ncases++;nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-depth";GPU(nvn_gpu_open(&od,&err));
    printf("feedback begin dq=%d opt=%d pool_fb=%d case=%d\n",dq_mode,opt_mode,pool_fb_mode,which);
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
#ifdef S2A_NVK_POOL_HOST_TEST
// 案例 20（整池组回收）：反馈补丁必须在段完成时还原。同一局里连提两段：
//   第 1 段 = 案例 16 的录法（池里登记的 0 号附件本笔没写：每笔拷快照、把池里的两项换成快照视图 = 补丁；段完成时快照视图销毁，补丁要先写回）；
//   第 2 段 = 同一张池快照、同一个采样器，附件换成另一张图（与池里的图不重叠、不拷快照）：取回第 1 段放回的整池组，里面采样的必须是原图的视图，不能还是第 1 段已销毁的快照视图。
// 回收读数核「第 2 段真的取回了回收组」（rec-hit = 1），免得用例空转；补丁项数 = 2 笔 × 池里 2 项 = 4。
static void run_rec_restore(struct s2a_vk_device *device,const char *dir) {
    observer_reset();begins=0;skip_logs=0;ncases++;nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-rec-restore";GPU(nvn_gpu_open(&od,&err));
    printf("feedback begin dq=%d opt=%d pool_fb=%d case=20\n",dq_mode,opt_mode,pool_fb_mode);
    nvn_gpu_layout_ent ent[2]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].access=NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    ent[0].count=2;ent[0].src_kind=NVN_RB_TEXP;observed_expect_samples=2;
    nvn_gpu_layout layout={0};layout.ent=ent;layout.nent=2;
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,"feedback_pool.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=240;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex images[2]={0};nvn_gpu_view views[2]={0},sample=NULL;
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;td.d.depth=1;td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    view_t av={0};av.set_layers=1;av.nlayers=1;av.min_layer=0;av.set_target=1;av.target=NVN_TEXTURE_TARGET_2D;
    for(unsigned i=0;i<2;i++){GPU(nvn_gpu_tex_create(&td,&images[i],&err));GPU(nvn_gpu_view_create(images[i],&av,&views[i],&err));}
    view_t v={0};v.set_levels=1;v.nlevels=1;v.base_level=0;
    GPU(nvn_gpu_view_create(images[0],&v,&sample,&err));
    samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));
    nvn_gpu_pool_table pool=NULL;
    {nvn_gpu_pool_item item={.state=NVN_GPU_POOL_READY};item.resource.view=sample;nvn_gpu_pool_desc d={.kind=NVN_GPU_POOL_IMAGE,.count=1,.items=&item};d.dummy.view=sample;GPU(nvn_gpu_pool_create(&d,&pool,&err));}
    nvn_gpu_binding sb={0};sb.ent=1;sb.cls=NVN_GPU_RES_SAMP;sb.u.samp=sampler;
    nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;
    const uint64_t hit0=nvn_gpu_vk_test_pool_sets(5),patch0=nvn_gpu_vk_test_pool_sets(9);uint64_t hit_seg1=0;
    for(int seg=0;seg<2;seg++){
        // 栅栏到了不等于完成线程把上一段做完（放回回收组在 finish_cmd 里）：开下一段前等现存组都放回
        for(int w=0;w<2000&&nvn_gpu_vk_test_prs(0)!=nvn_gpu_vk_test_prs(1);w++){struct timespec ts={0,1000000};nanosleep(&ts,NULL);}
        if(seg)hit_seg1=nvn_gpu_vk_test_pool_sets(5)-hit0;
        nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
        if(!seg)for(unsigned i=0;i<2;i++){begin_pass(cmd,views[i],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_pass_end(cmd);}
        begin_pass(cmd,views[seg],NVN_GPU_LOAD_LOAD,4);
        nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_set_dyn(cmd,&dyn);
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&sb,1);GPU(nvn_gpu_pool_bind(cmd,NVN_GPU_SH_FS,0,pool,&err));
        nvn_gpu_draw(cmd,&a);nvn_gpu_draw(cmd,&a);
        nvn_gpu_pass_end(cmd);
        GPU(nvn_gpu_cmd_commit(cmd,1+seg,done,NULL,&err));GPU(nvn_gpu_fence_wait(1+seg,5000000000ull,&err));
    }
    REQUIRE(nvn_gpu_fence_failed()==0);
    const uint64_t hit_seg2=nvn_gpu_vk_test_pool_sets(5)-hit0-hit_seg1,patches=nvn_gpu_vk_test_pool_sets(9)-patch0;
    unsigned draws_bad=observed_draws!=4,copy_bad=observed_copies!=2,dead_bad=observed_dead_samples!=0,overlap_bad=observed_overlap!=0;
    unsigned hit_bad=!(hit_seg1==0&&hit_seg2==1),patch_bad=patches!=4;   // 第 1 段：空局里没有回收组可取（0 次命中）；第 2 段：取回 1 个
    printf("feedback rec-restore dq=%d opt=%d pool_fb=%d case=20 draws=%u copies=%u dead-samples=%u overlap=%u rec-hit=%llu/%llu patches=%llu draws-bad=%u copy-bad=%u dead-bad=%u overlap-bad=%u hit-bad=%u patch-bad=%u\n",
           dq_mode,opt_mode,pool_fb_mode,observed_draws,observed_copies,observed_dead_samples,observed_overlap,(unsigned long long)hit_seg1,(unsigned long long)hit_seg2,(unsigned long long)patches,
           draws_bad,copy_bad,dead_bad,overlap_bad,hit_bad,patch_bad);
    failures+=draws_bad+copy_bad+dead_bad+overlap_bad+hit_bad+patch_bad;
    nvn_gpu_pool_release(pool);nvn_gpu_samp_release(sampler);nvn_gpu_view_release(sample);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));
    REQUIRE(observed_early_destroy==0);
}
#endif
int main(int argc,char **argv) {
    if(argc!=6)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);   // 行缓冲：校验层的报错和 begin / 结果行按真实先后进同一份日志（脚本按 begin 行把报错归到案例）
    dq_mode=atoi(argv[2]);opt_mode=atoi(argv[3]);REQUIRE((dq_mode==0||dq_mode==1)&&(opt_mode==0||opt_mode==1));
    copy_mode=atoi(argv[4]);REQUIRE(copy_mode==0||copy_mode==1);
    pool_fb_mode=atoi(argv[5]);REQUIRE(pool_fb_mode==0||pool_fb_mode==1);
    observed_on_begin=on_begin;
    struct s2a_vk_device device;char why[256];
    if(s2a_vk_device_create(&device,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
    const char *diag = getenv("S2A_TEST_PASS_DIAG");
    if (diag) {
        diag_pass = !strcmp(diag, "pass") || !strcmp(diag, "both");
        diag_snapshot = !strcmp(diag, "snapshot") || !strcmp(diag, "both");
        run_case(&device, argv[1], 0);  // 3 passes, no self-sampling
        run_case(&device, argv[1], 3);  // 5 passes, 2 RGBA8 copies of 4 x 4
        run_depth(&device, argv[1], NVN_FORMAT_DEPTH32F_STENCIL8, 13);
        run_case(&device, argv[1], 14); // records a copy but aborts before submission
        REQUIRE(!s2a_vk_device_destroy(&device, 1));
        printf("feedback cases=%u failures=%u\n", ncases, failures);
        return failures ? 1 : 0;
    }
    // 守卫探针先跑（只录制不提交）：免拷的前提（GENERAL 档、没开旧行为开关）被撤掉时，在后面真执行的案例把驱动跑崩之前就按读数变红。
    run_case(&device,argv[1],18);
    if(failures&&copy_mode){printf("feedback cases=%u failures=%u\n",ncases,failures);return 1;}   // 快照整条路径关掉的负控（copy_mode=0）要继续跑到真执行的案例，不在这里收
    for(int c=0;c<12;c++)run_case(&device,argv[1],c);
    run_depth(&device,argv[1],NVN_FORMAT_DEPTH32F,12);
    run_depth(&device,argv[1],NVN_FORMAT_DEPTH32F_STENCIL8,13);
    run_case(&device,argv[1],14);
    // 案例 15（池里别的图 + 一个只登记不读的附件）、19（同样的池，着色器按运行时下标真读那个附件）要求池里的图都是 GENERAL：
    // nvk_opt_layout=1 时 pool_check_layouts 按设计拒绝其余布局，这两个案例该模式不适用（opt=1 下免拷的守卫由案例 18 钉住）。
    if(!opt_mode){run_case(&device,argv[1],15);run_case(&device,argv[1],19);}
    run_case(&device,argv[1],16);
    run_case(&device,argv[1],17);
#ifdef S2A_NVK_POOL_HOST_TEST
    run_rec_restore(&device,argv[1]);   // 案例 20：整池组回收，反馈补丁段完成时还原
#endif
    REQUIRE(!s2a_vk_device_destroy(&device,1));printf("feedback cases=%u failures=%u\n",ncases,failures);return failures?1:0;
}
