// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 反馈快照池（backend_vulkan_feedback.inc 的 fbpool_*）：快照图跨段复用。
// 每段：CLEAR 附件到红 = k/255，再在 LOAD 通道里画一笔「采样同一张图 + 0.25」（读写同一附件 → 后端拷快照），读回前 4 个像素。
// 期望值手算：红 = k + 63.75 四舍五入（k = 32 → 96，k = 160 → 224）；绿蓝 0；alpha 255。
// 建图 / 要显存次数用 --wrap 数真实驱动调用；贴图显存池（nvk_texmem）关掉，让 vkAllocateMemory 只数真分配。
#include "nvn_backend.h"
#include "nvn_plat_vk.h"
#include "vk_device.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static nvn_gpu_err err;
static int dq_mode, pool_mb = 256;
static unsigned failures;
static uint64_t next_signal;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"fixture failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
#define CHECK(cond, ...) do { if(!(cond)){failures++;printf("  【红】");printf(__VA_ARGS__);printf("\n");} } while(0)
void nvn_vk_fbpool_stat(uint64_t out[5]);
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){
    if(!strcmp(n,"nvk_dq"))return dq_mode;
    if(!strcmp(n,"nvk_texmem"))return 0;
    if(!strcmp(n,"nvk_feedback_pool_mb"))return pool_mb;
    return d;
}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static atomic_uint n_create_image, n_alloc;
extern __typeof__(vkCreateImage) __real_vkCreateImage;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImage(VkDevice d,const VkImageCreateInfo *p,const VkAllocationCallbacks *a,VkImage *o){
    atomic_fetch_add(&n_create_image,1);return __real_vkCreateImage(d,p,a,o);
}
extern __typeof__(vkAllocateMemory) __real_vkAllocateMemory;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *p,const VkAllocationCallbacks *a,VkDeviceMemory *o){
    atomic_fetch_add(&n_alloc,1);return __real_vkAllocateMemory(d,p,a,o);
}
static void done(void *u,const nvn_gpu_done *d){(void)u;REQUIRE(d->status==NVN_GPU_DONE_OK);}
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout) {
    char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
    FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
    void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
    nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
// 一个会话里的公共对象
static nvn_gpu_shader g_vs,g_fs;static nvn_gpu_pipe g_pipe;static nvn_gpu_samp g_samp;
struct target { nvn_gpu_tex tex; nvn_gpu_view att, smp; nvn_gpu_buf rb; unsigned w, h; };
static void open_session(struct s2a_vk_device *dev,const char *dir,int mb){
    pool_mb=mb;next_signal=0;
    nvn_gpu_open_desc od={0};od.host_device=dev;od.app_label="feedback-snap-pool";GPU(nvn_gpu_open(&od,&err));
    static nvn_gpu_layout_ent ent[2];memset(ent,0,sizeof ent);ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].access=NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    static nvn_gpu_layout layout;memset(&layout,0,sizeof layout);layout.ent=ent;layout.nent=2;
    g_vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL);g_fs=shader(dir,"feedback.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;
    pd.st[NVN_GPU_SH_VS].sh=g_vs;pd.st[NVN_GPU_SH_FS].sh=g_fs;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    g_pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&g_pipe,&err));
    samp_par sp={0};g_samp=NULL;GPU(nvn_gpu_samp_create(&sp,&g_samp,&err));
}
static void close_session(void){
    nvn_gpu_samp_release(g_samp);nvn_gpu_shader_release(g_vs);nvn_gpu_shader_release(g_fs);
    GPU(nvn_gpu_close(5000000000ull,&err));
}
static uint32_t target_extra_usage;   // wantview 用例给目标追 VIEW_FMT（声明过异格式视图资格 → want_view → 出生非 MUTABLE）
static struct target make_target(unsigned w,unsigned h){
    struct target t={0};t.w=w;t.h=h;
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=(int)w;td.d.height=(int)h;
    td.d.depth=1;td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED|target_extra_usage;
    GPU(nvn_gpu_tex_create(&td,&t.tex,&err));GPU(nvn_gpu_view_create(t.tex,NULL,&t.att,&err));GPU(nvn_gpu_view_create(t.tex,NULL,&t.smp,&err));
    GPU(nvn_gpu_buf_create(16,NVN_GPU_MEM_SHARED,NULL,&t.rb,&err));
    return t;
}
static void drop_target(struct target *t){
    nvn_gpu_buf_release(t->rb,NULL,NULL);nvn_gpu_view_release(t->att);nvn_gpu_view_release(t->smp);nvn_gpu_tex_release(t->tex);
}
static void stat_now(uint64_t s[5]){nvn_vk_fbpool_stat(s);}
// 一段：每个目标依次 CLEAR 到 red/255、再画一笔反馈采样，读回前 4 像素。commit = 0 时 abort（不执行）。
static void seg(struct target *ts,unsigned n,unsigned red,int commit){
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    for(unsigned i=0;i<n;i++){
        struct target *t=&ts[i];
        nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=t->att;p.color[0].store=NVN_GPU_STORE_STORE;p.w=t->w;p.h=t->h;p.layers=1;
        p.color[0].load=NVN_GPU_LOAD_CLEAR;p.color[0].clear.f[0]=(float)red/255.0f;p.color[0].clear.f[3]=1;
        nvn_gpu_pass_begin(cmd,&p);nvn_gpu_pass_end(cmd);
        p.color[0].load=NVN_GPU_LOAD_LOAD;nvn_gpu_pass_begin(cmd,&p);
        nvn_gpu_set_pipe(cmd,g_pipe);
        nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=(float)t->w;dyn.vp[0][3]=(float)t->h;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=(int)t->w;dyn.sc[3]=(int)t->h;
        nvn_gpu_set_dyn(cmd,&dyn);
        nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=t->smp;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=g_samp;
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,b,2);
        nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;nvn_gpu_draw(cmd,&a);
        nvn_gpu_pass_end(cmd);
        NVNcopyRegion r={0,0,0,4,1,1};nvn_gpu_copy_tex_buf(cmd,t->tex,0,0,&r,t->rb,0,16,16);
    }
    if(!commit){nvn_gpu_cmd_abort(cmd);return;}
    next_signal++;GPU(nvn_gpu_cmd_commit(cmd,next_signal,done,NULL,&err));GPU(nvn_gpu_fence_wait(next_signal,5000000000ull,&err));
}
// 栅栏到了之后完成线程才 finish_cmd（放回池）：等池里张数到 want（最多 2 秒）
static int settle(uint64_t want){
    for(int i=0;i<2000;i++){uint64_t s[5];stat_now(s);if(s[3]==want)return 1;struct timespec ts={0,1000000};nanosleep(&ts,NULL);}
    return 0;
}
static unsigned px_bad(struct target *t,unsigned red){
    const unsigned char *b=nvn_gpu_buf_cpu(t->rb);unsigned bad=0;
    for(unsigned i=0;i<4;i++)if(b[i*4]!=red||b[i*4+1]||b[i*4+2]||b[i*4+3]!=255)bad++;
    return bad;
}
static unsigned red0(struct target *t){return nvn_gpu_buf_cpu(t->rb)[0];}

// A：同源同形状两段 → 第二段池命中，不建图、不要显存；第二段读到的是自己拷的快照（224），不是池里旧内容（96）
static void case_hit(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,256);struct target t=make_target(4,4);uint64_t s0[5],s1[5],s2[5];stat_now(s0);
    seg(&t,1,32,1);unsigned b1=px_bad(&t,96),r1=red0(&t);
    CHECK(settle(1),"hit：第一段完成后池里应有 1 张");stat_now(s1);
    unsigned ci=atomic_load(&n_create_image),ca=atomic_load(&n_alloc);
    seg(&t,1,160,1);unsigned b2=px_bad(&t,224);
    unsigned dci=atomic_load(&n_create_image)-ci,dca=atomic_load(&n_alloc)-ca;
    CHECK(settle(1),"hit：第二段完成后池里应有 1 张");stat_now(s2);
    printf("fbpool dq=%d case=hit new=%llu hit=%llu evict=%llu seg2-create-image=%u seg2-alloc=%u red1=%u red2=%u pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),(unsigned long long)(s2[2]-s0[2]),dci,dca,r1,red0(&t),b1+b2);
    CHECK(s2[0]-s0[0]==1&&s2[1]-s0[1]==1&&s2[2]-s0[2]==0,"hit：期望 新建 1 / 命中 1 / 淘汰 0");
    CHECK(dci==0&&dca==0,"hit：第二段期望 vkCreateImage 0 次、vkAllocateMemory 0 次，实为 %u / %u",dci,dca);
    CHECK(b1==0,"hit：第一段像素期望红 96");CHECK(b2==0,"hit：第二段像素期望红 224（读到 96 = 采样了池里旧内容）");
    drop_target(&t);close_session();
}
// B：形状只差宽度（4x4 → 8x4）不复用
static void case_shape(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,256);struct target a=make_target(4,4),b=make_target(8,4);uint64_t s0[5],s2[5];stat_now(s0);
    seg(&a,1,32,1);unsigned b1=px_bad(&a,96);CHECK(settle(1),"shape：池里应有 1 张");
    unsigned ci=atomic_load(&n_create_image);
    seg(&b,1,160,1);unsigned b2=px_bad(&b,224);unsigned dci=atomic_load(&n_create_image)-ci;
    CHECK(settle(2),"shape：两种形状各 1 张");stat_now(s2);
    printf("fbpool dq=%d case=shape new=%llu hit=%llu seg2-create-image=%u pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),dci,b1+b2);
    CHECK(s2[0]-s0[0]==2&&s2[1]-s0[1]==0,"shape：期望 新建 2 / 命中 0");CHECK(dci==1,"shape：第二段期望新建 1 张快照图，实为 %u",dci);
    CHECK(b1+b2==0,"shape：像素期望 96 / 224");
    drop_target(&a);drop_target(&b);close_session();
}
// G：源声明 VIEW_FMT + 绑定 sRGB 孪生视图 → 反馈快照要按 sRGB 建视图。快照照源带 VIEW_FMT 资格、出生非 MUTABLE，
//   视图创建点必须走 tex_upgrade_mutable 按需重建（修前：直调 vkCreateImageView 绕过闸 → VUID-01762 或具名拒绝后红）。
//   b.smp 是 sRGB 视图：采样按 sRGB 解码，附件读回值与 UNORM 期望不同——像素断言按「值变了」只记不判。
static void case_wantview(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,256);struct target a=make_target(4,4);
    target_extra_usage=NVN_GPU_TEX_VIEW_FMT;struct target b=make_target(4,4);target_extra_usage=0;
    nvn_gpu_view_release(b.smp);
    view_t srgb={0};srgb.set_format=1;srgb.format=NVN_FORMAT_RGBA8_SRGB;
    GPU(nvn_gpu_view_create(b.tex,&srgb,&b.smp,&err));
    uint64_t s0[5],s2[5];stat_now(s0);
    seg(&a,1,32,1);unsigned b1=px_bad(&a,96);CHECK(settle(1),"wantview：池里应有 1 张");
    unsigned ci=atomic_load(&n_create_image);
    seg(&b,1,160,1);unsigned b2=px_bad(&b,224);unsigned dci=atomic_load(&n_create_image)-ci;
    CHECK(settle(2),"wantview：两种各 1 张");stat_now(s2);
    printf("fbpool dq=%d case=wantview new=%llu hit=%llu seg2-create-image=%u pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),dci,b1+b2);
    CHECK(s2[0]-s0[0]==2&&s2[1]-s0[1]==0,"wantview：期望 新建 2 / 命中 0");
    CHECK(dci==2,"wantview：第二段期望新建 2 张图（快照 1 + 按需重建 1），实为 %u",dci);
    drop_target(&a);drop_target(&b);close_session();
}
// C：开关 0 = 现状：每段新建、段完成即销毁，池恒空
static void case_off(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,0);struct target t=make_target(4,4);uint64_t s0[5],s2[5];stat_now(s0);
    seg(&t,1,32,1);unsigned b1=px_bad(&t,96);
    unsigned ci=atomic_load(&n_create_image),ca=atomic_load(&n_alloc);
    seg(&t,1,160,1);unsigned b2=px_bad(&t,224);
    unsigned dci=atomic_load(&n_create_image)-ci,dca=atomic_load(&n_alloc)-ca;stat_now(s2);
    printf("fbpool dq=%d case=off new=%llu hit=%llu pool=%llu seg2-create-image=%u seg2-alloc=%u pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),(unsigned long long)s2[3],dci,dca,b1+b2);
    CHECK(s2[0]-s0[0]==2&&s2[1]-s0[1]==0&&s2[3]==0,"off：期望 新建 2 / 命中 0 / 池空");
    CHECK(dci==1&&dca==1,"off：第二段期望 vkCreateImage 1、vkAllocateMemory 1，实为 %u / %u",dci,dca);
    CHECK(b1+b2==0,"off：像素期望 96 / 224");
    drop_target(&t);close_session();
}
// D：上限 1 MB：256x256（256 KiB）进池后，448x448（784 KiB）放回时总和 1040 KiB 超上限 → 淘汰前者；再做 256x256 → 新建
static void case_evict(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,1);struct target a=make_target(256,256),b=make_target(448,448);uint64_t s0[5],s1[5],s2[5];stat_now(s0);
    seg(&a,1,32,1);unsigned bad=px_bad(&a,96);CHECK(settle(1),"evict：池里应有 1 张");
    seg(&b,1,160,1);bad+=px_bad(&b,224);
    int ok=0;for(int i=0;i<2000&&!ok;i++){stat_now(s1);ok=s1[2]-s0[2]==1&&s1[3]==1;if(!ok){struct timespec ts={0,1000000};nanosleep(&ts,NULL);}}
    CHECK(ok,"evict：放回 448x448 时应淘汰 1 张、池里剩 1 张，实为 淘汰 %llu / 池 %llu",(unsigned long long)(s1[2]-s0[2]),(unsigned long long)s1[3]);
    CHECK(s1[4]<=(1ull<<20),"evict：池字节 %llu 超上限",(unsigned long long)s1[4]);
    seg(&a,1,160,1);bad+=px_bad(&a,224);
    ok=0;for(int i=0;i<2000&&!ok;i++){stat_now(s2);ok=s2[2]-s0[2]==2;if(!ok){struct timespec ts={0,1000000};nanosleep(&ts,NULL);}}
    printf("fbpool dq=%d case=evict new=%llu hit=%llu evict=%llu pool-bytes=%llu pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),(unsigned long long)(s2[2]-s0[2]),(unsigned long long)s2[4],bad);
    CHECK(s2[0]-s0[0]==3&&s2[1]-s0[1]==0&&s2[2]-s0[2]==2&&s2[3]==1&&s2[4]==262144,"evict：期望 新建 3 / 命中 0 / 淘汰 2（第三段放回时 784 + 256 KiB 又超）/ 池里 1 张 262144 字节");
    CHECK(bad==0,"evict：像素期望 96 / 224 / 224");
    drop_target(&a);drop_target(&b);close_session();
}
// E：段没执行（abort）的新建快照不进池；下一段照旧新建
static void case_abort(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,256);struct target t=make_target(4,4);uint64_t s0[5],s1[5],s2[5];stat_now(s0);
    seg(&t,1,32,0);stat_now(s1);
    seg(&t,1,160,1);unsigned bad=px_bad(&t,224);settle(1);stat_now(s2);
    printf("fbpool dq=%d case=abort pool-after-abort=%llu new=%llu hit=%llu pixel-bad=%u\n",dq_mode,
        (unsigned long long)s1[3],(unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),bad);
    CHECK(s1[3]==0,"abort：放弃的段的快照不应进池");CHECK(s2[0]-s0[0]==2&&s2[1]-s0[1]==0,"abort：期望 新建 2 / 命中 0");
    CHECK(bad==0,"abort：像素期望 224");
    drop_target(&t);close_session();
}
// F：同一段里两张同形状的源各要一张快照：池里没有就新建 2 张；下一段两张都命中，各自读到自己的内容
static void case_two(struct s2a_vk_device *dev,const char *dir){
    open_session(dev,dir,256);struct target t[2]={make_target(4,4),make_target(4,4)};uint64_t s0[5],s1[5],s2[5];stat_now(s0);
    seg(t,2,32,1);unsigned bad=px_bad(&t[0],96)+px_bad(&t[1],96);CHECK(settle(2),"two：池里应有 2 张");stat_now(s1);
    unsigned ci=atomic_load(&n_create_image);
    seg(t,2,160,1);bad+=px_bad(&t[0],224)+px_bad(&t[1],224);unsigned dci=atomic_load(&n_create_image)-ci;settle(2);stat_now(s2);
    printf("fbpool dq=%d case=two seg1-new=%llu new=%llu hit=%llu seg2-create-image=%u pixel-bad=%u\n",dq_mode,
        (unsigned long long)(s1[0]-s0[0]),(unsigned long long)(s2[0]-s0[0]),(unsigned long long)(s2[1]-s0[1]),dci,bad);
    CHECK(s1[0]-s0[0]==2&&s2[0]-s0[0]==2&&s2[1]-s0[1]==2&&dci==0,"two：期望 第一段新建 2、第二段命中 2、不建图");
    CHECK(bad==0,"two：像素期望 96,96 / 224,224");
    drop_target(&t[0]);drop_target(&t[1]);close_session();
}
int main(int argc,char **argv){
    if(argc!=3)return 2;
    dq_mode=atoi(argv[2]);REQUIRE(dq_mode==0||dq_mode==1);
    struct s2a_vk_device device;char why[256];
    if(s2a_vk_device_create(&device,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
    case_hit(&device,argv[1]);case_shape(&device,argv[1]);case_wantview(&device,argv[1]);case_off(&device,argv[1]);
    case_evict(&device,argv[1]);case_abort(&device,argv[1]);case_two(&device,argv[1]);
    REQUIRE(!s2a_vk_device_destroy(&device,1));printf("fbpool cases=7 failures=%u\n",failures);return failures?1:0;
}
