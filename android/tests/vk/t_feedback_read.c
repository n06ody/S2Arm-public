// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Reuse C3's real resource observer and setup helpers, not backend internals.
#define main snapshot_fixture_main
#define nvn_plat_flag snapshot_plat_flag
#include "t_feedback.c"
#undef main
#undef nvn_plat_flag
static int read_mode, hide_extension, enabled_none, read_trace;
static unsigned read_cases, read_bad_pass, read_bad_write, read_live, read_np;
static unsigned read_check_layout, read_bad_layout, read_next_clear;
static VkRenderPass read_active;
static struct { VkRenderPass pass; VkAttachmentDescription a[9]; unsigned n,ncol,cols[8],ds; } read_passes[256];
static struct { VkPipelineLayout layout; unsigned nsets; } read_layouts[64];
static struct { VkPipeline pipe; VkShaderModule fs; unsigned nsets,ncol,mask[8]; VkPipelineDepthStencilStateCreateInfo ds; } read_pipes[64];
static unsigned read_nlayout, read_npipe, read_bound_sets, read_bound_pipe, read_stencil_masks[2];
extern __typeof__(vkCreatePipelineLayout) __real_vkCreatePipelineLayout;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *p,
        const VkAllocationCallbacks *a,VkPipelineLayout *out) {
    VkResult r=__real_vkCreatePipelineLayout(d,p,a,out);pthread_mutex_lock(&observed_mu);
    if(r==VK_SUCCESS){unsigned i=0;while(i<read_nlayout&&read_layouts[i].layout!=*out)i++;
        if(i==read_nlayout){REQUIRE(i<64);read_nlayout++;}read_layouts[i].layout=*out;read_layouts[i].nsets=p->setLayoutCount;}
    pthread_mutex_unlock(&observed_mu);return r;
}
extern __typeof__(vkCreateGraphicsPipelines) __real_vkCreateGraphicsPipelines;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache cache,uint32_t n,
        const VkGraphicsPipelineCreateInfo *p,const VkAllocationCallbacks *a,VkPipeline *out) {
    VkResult r=__real_vkCreateGraphicsPipelines(d,cache,n,p,a,out);pthread_mutex_lock(&observed_mu);
    for(uint32_t j=0;j<n;j++)if(out[j]){
        unsigned l=0;while(l<read_nlayout&&read_layouts[l].layout!=p[j].layout)l++;REQUIRE(l<read_nlayout);
        unsigned i=0;while(i<read_npipe&&read_pipes[i].pipe!=out[j])i++;
        if(i==read_npipe){REQUIRE(i<64);read_npipe++;}read_pipes[i].pipe=out[j];read_pipes[i].nsets=read_layouts[l].nsets;
        REQUIRE(p[j].pColorBlendState&&p[j].pColorBlendState->attachmentCount<=8);
        read_pipes[i].ncol=p[j].pColorBlendState->attachmentCount;
        for(unsigned k=0;k<read_pipes[i].ncol;k++)read_pipes[i].mask[k]=p[j].pColorBlendState->pAttachments[k].colorWriteMask;
        memset(&read_pipes[i].ds,0,sizeof read_pipes[i].ds);
        if(p[j].pDepthStencilState)read_pipes[i].ds=*p[j].pDepthStencilState;
        for(unsigned k=0;k<p[j].stageCount;k++)if(p[j].pStages[k].stage==VK_SHADER_STAGE_FRAGMENT_BIT)read_pipes[i].fs=p[j].pStages[k].module;
    }
    pthread_mutex_unlock(&observed_mu);return r;
}
extern __typeof__(vkCmdBindPipeline) __real_vkCmdBindPipeline;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p) {
    if(b==VK_PIPELINE_BIND_POINT_GRAPHICS){pthread_mutex_lock(&observed_mu);
        unsigned i=0;while(i<read_npipe&&read_pipes[i].pipe!=p)i++;REQUIRE(i<read_npipe);
        read_bound_pipe=i;read_bound_sets=read_pipes[i].nsets;REQUIRE(read_bound_sets<=16);pthread_mutex_unlock(&observed_mu);}
    __real_vkCmdBindPipeline(c,b,p);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreatePipelineLayout),PFN_vkCreatePipelineLayout),"NDK layout signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateGraphicsPipelines),PFN_vkCreateGraphicsPipelines),"NDK pipeline signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdBindPipeline),PFN_vkCmdBindPipeline),"NDK bind signature");
extern __typeof__(vkCmdSetStencilWriteMask) __real_vkCmdSetStencilWriteMask;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdSetStencilWriteMask(VkCommandBuffer c,VkStencilFaceFlags face,uint32_t mask){
    if(face&VK_STENCIL_FACE_FRONT_BIT)read_stencil_masks[0]=mask;
    if(face&VK_STENCIL_FACE_BACK_BIT)read_stencil_masks[1]=mask;
    __real_vkCmdSetStencilWriteMask(c,face,mask);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdSetStencilWriteMask),PFN_vkCmdSetStencilWriteMask),"NDK stencil mask signature");
static unsigned read_sample_sets(void){return read_bound_sets;}
static PFN_vkVoidFunction read_command(const char *name){
    if(!strcmp(name,"vkCmdSetStencilWriteMask"))return (PFN_vkVoidFunction)__wrap_vkCmdSetStencilWriteMask;
    return !strcmp(name,"vkCmdBindPipeline")?(PFN_vkVoidFunction)__wrap_vkCmdBindPipeline:NULL;
}
int nvn_plat_flag(const char *name,int dflt) {
    if(!strcmp(name,"nvk_fb_read"))return read_mode;
    return snapshot_plat_flag(name,dflt);
}
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *p,
        const VkAllocationCallbacks *a,VkRenderPass *out) {
    VkResult r=__real_vkCreateRenderPass(d,p,a,out);
    pthread_mutex_lock(&observed_mu);
    if(r==VK_SUCCESS){unsigned k=0;while(k<read_np&&read_passes[k].pass!=*out)k++;
        if(k==read_np){REQUIRE(read_np<256);read_np++;}REQUIRE(p->attachmentCount<=9);
        read_passes[k].pass=*out;read_passes[k].n=p->attachmentCount;
        REQUIRE(p->subpassCount==1&&p->pSubpasses[0].colorAttachmentCount<=8);
        read_passes[k].ncol=p->pSubpasses[0].colorAttachmentCount;
        for(unsigned i=0;i<read_passes[k].ncol;i++)read_passes[k].cols[i]=p->pSubpasses[0].pColorAttachments[i].attachment;
        read_passes[k].ds=p->pSubpasses[0].pDepthStencilAttachment?p->pSubpasses[0].pDepthStencilAttachment->attachment:VK_ATTACHMENT_UNUSED;
        if(p->attachmentCount)memcpy(read_passes[k].a,p->pAttachments,p->attachmentCount*sizeof *p->pAttachments);}
    pthread_mutex_unlock(&observed_mu);return r;
}
extern __typeof__(vkCreateDevice) __real_vkCreateDevice;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo *ci,
        const VkAllocationCallbacks *a,VkDevice *d) {
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++)
        if(!strcmp(ci->ppEnabledExtensionNames[i],VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))enabled_none=1;
    return __real_vkCreateDevice(p,ci,a,d);
}
extern __typeof__(vkEnumerateDeviceExtensionProperties) __real_vkEnumerateDeviceExtensionProperties;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *layer,
        uint32_t *count,VkExtensionProperties *props) {
    if(!hide_extension||layer)return __real_vkEnumerateDeviceExtensionProperties(p,layer,count,props);
    // Strict two-call enumeration: count only exposed entries, preserve capacity
    // and VK_INCOMPLETE semantics. Never add an extension the driver lacks.
    uint32_t n=0;VkResult r=__real_vkEnumerateDeviceExtensionProperties(p,layer,&n,NULL);
    if(r!=VK_SUCCESS)return r;
    VkExtensionProperties *all=calloc(n?n:1,sizeof *all);REQUIRE(all);
    r=__real_vkEnumerateDeviceExtensionProperties(p,layer,&n,all);
    if(r!=VK_SUCCESS){free(all);return r;}
    uint32_t visible=0;
    for(uint32_t i=0;i<n;i++)if(strcmp(all[i].extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))all[visible++]=all[i];
    if(!props){*count=visible;free(all);return VK_SUCCESS;}
    uint32_t written=*count<visible?*count:visible;
    memcpy(props,all,written*sizeof *all);*count=written;free(all);
    return written<visible?VK_INCOMPLETE:VK_SUCCESS;
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass),PFN_vkCreateRenderPass),"NDK pass signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateDevice),PFN_vkCreateDevice),"NDK device signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkEnumerateDeviceExtensionProperties),PFN_vkEnumerateDeviceExtensionProperties),"NDK enumeration signature");
static void read_begin(const VkRenderPassBeginInfo *p){
    read_active=p->renderPass;
    pthread_mutex_lock(&observed_mu);
    unsigned k=0;while(k<read_np&&read_passes[k].pass!=p->renderPass)k++;REQUIRE(k<read_np);
    if(read_check_layout&&observed_draws==1&&read_passes[k].n&&read_passes[k].a[0].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR){
        read_next_clear++;
        VkImageLayout want=opt_mode?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
        read_bad_layout+=read_passes[k].a[0].initialLayout!=want;
    }
    pthread_mutex_unlock(&observed_mu);
}
static void read_draw(void) {
    unsigned p=0,f=0;while(p<read_np&&read_passes[p].pass!=read_active)p++;
    while(f<observed_nf&&observed_fbs[f].fb!=observed_fb)f++;
    REQUIRE(p<read_np&&f<observed_nf&&read_passes[p].n==observed_fbs[f].n);
    REQUIRE(read_bound_pipe<read_npipe);
    for(unsigned i=0;i<read_passes[p].ncol;i++){
        unsigned a=read_passes[p].cols[i];if(a==VK_ATTACHMENT_UNUSED)continue;
        REQUIRE(i<read_pipes[read_bound_pipe].ncol&&a<read_passes[p].n);
        if(read_pipes[read_bound_pipe].mask[i])read_bad_write+=read_passes[p].a[a].storeOp==VK_ATTACHMENT_STORE_OP_NONE_EXT;
    }
    unsigned ds=read_passes[p].ds;
    if(ds!=VK_ATTACHMENT_UNUSED){
        const VkPipelineDepthStencilStateCreateInfo *d=&read_pipes[read_bound_pipe].ds;
        if(d->depthTestEnable&&d->depthWriteEnable)read_bad_write+=read_passes[p].a[ds].storeOp==VK_ATTACHMENT_STORE_OP_NONE_EXT;
        VkStencilOpState face[2]={d->front,d->back};
        for(unsigned i=0;i<2;i++)if(d->stencilTestEnable&&(read_stencil_masks[i]&255)&&
            (face[i].failOp!=VK_STENCIL_OP_KEEP||face[i].passOp!=VK_STENCIL_OP_KEEP||face[i].depthFailOp!=VK_STENCIL_OP_KEEP))
            read_bad_write+=read_passes[p].a[ds].stencilStoreOp==VK_ATTACHMENT_STORE_OP_NONE_EXT;
    }
    if(read_trace)printf("mask-repro draw=%u pipe=%llu fs=%llu color-mask0=%u sampled-sets=%u\n",observed_draws,
        (unsigned long long)(uintptr_t)read_pipes[read_bound_pipe].pipe,(unsigned long long)(uintptr_t)read_pipes[read_bound_pipe].fs,
        read_pipes[read_bound_pipe].mask[0],read_bound_sets);
    for(unsigned s=0;s<read_bound_sets;s++)if(observed_bound[s]){
        unsigned set=observed_set(observed_bound[s]);
        for(unsigned b=0;b<4;b++)for(unsigned e=0;e<8;e++){
            VkImageView view=observed_sets[set].views[b][e];if(!view)continue;unsigned v=observed_view(view);
            for(unsigned k=0;k<observed_fbs[f].n;k++){
                unsigned a=observed_view(observed_fbs[f].views[k]);
                if(observed_views[v].image!=observed_views[a].image||!observed_intersects(observed_views[v].range,observed_views[a].range))continue;
                const VkAttachmentDescription *d=&read_passes[p].a[k];read_live++;
                read_bad_pass+=d->loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD||d->storeOp!=VK_ATTACHMENT_STORE_OP_NONE_EXT;
                if(d->format==VK_FORMAT_D32_SFLOAT_S8_UINT||d->format==VK_FORMAT_D24_UNORM_S8_UINT)
                    read_bad_pass+=d->stencilLoadOp!=VK_ATTACHMENT_LOAD_OP_LOAD||d->stencilStoreOp!=VK_ATTACHMENT_STORE_OP_NONE_EXT;
            }
        }
    }
}
struct read_case { const char *name; int depth,stencil,sequence,load,discard; };
// sequence: read/read; write/write; write/read; read/write/read; read/clear/read; mask-only read.
static const struct read_case read_matrix[]={
    {"color-load",0,0,0,0,0},{"color-clear",0,0,0,1,0},{"color-discard",0,0,0,0,1},
    {"color-write",0,0,1,0,0},{"color-prior-write",0,0,2,0,0},
    {"color-transition",0,0,3,0,0},{"color-clear-rect",0,0,4,0,0},
    {"depth-read",1,0,0,1,0},{"depth-write",1,0,1,1,0},
    {"depth-stencil-write",1,1,0,1,0},{"depth-stencil-keep",1,2,0,1,0},
    {"depth-stencil-mask0",1,3,0,1,0},{"depth-transition",1,0,3,0,0},
    {"depth-clear-rect",1,0,4,0,0},
    {"color-mask-only",0,0,5,0,0},
    {"color-unsampled-write",0,0,7,0,0},{"depth-unsampled-write",1,0,7,0,0},
    {"color-next-pass-clear",0,0,8,0,0},
};
static void read_case_run(struct s2a_vk_device *device,const char *dir,const struct read_case *test) {
    observer_reset();read_np=read_bad_pass=read_bad_write=read_live=0;read_active=VK_NULL_HANDLE;
    read_nlayout=read_npipe=read_bound_sets=0;
    read_bound_pipe=64;read_stencil_masks[0]=read_stencil_masks[1]=255;
    read_check_layout=test->sequence==8;read_bad_layout=read_next_clear=0;
    nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="feedback-read";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent[2]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(unsigned i=0;i<2;i++){ent[i].count=1;ent[i].access=NVN_GPU_ACC_R;ent[i].stages=1u<<NVN_GPU_SH_FS;}
    nvn_gpu_layout layout={.ent=ent,.nent=2};
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs[2]={0};
    fs[0]=shader(dir,test->depth?"read_depth.frag":(test->sequence==5||test->sequence==6)?"read_color.frag":"read_only_color.frag",NVN_GPU_SH_FS,&layout);
    fs[1]=shader(dir,test->sequence==7?(test->depth?"write_depth.frag":"write_color.frag"):(test->depth?"read_depth.frag":"read_color.frag"),
        NVN_GPU_SH_FS,test->sequence==7?NULL:&layout);
    unsigned format=test->depth?(test->stencil?NVN_FORMAT_DEPTH32F_STENCIL8:NVN_FORMAT_DEPTH32F):NVN_FORMAT_RGBA8;
    nvn_gpu_pipe pipes[2]={0};
    for(unsigned write=0;write<2;write++){
        nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;
        pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs[write];pd.cfmt[0]=NVN_FORMAT_RGBA8;
        pd.chmask.mask=test->depth?15:(write?255:240);
        if(test->depth){pd.dfmt=format;pd.ds.dt=1;pd.ds.dw=write;pd.ds.df=8;
            if(test->stencil){pd.sfmt=format;pd.ds.st=1;pd.ds.sf_f=pd.ds.sf_b=8;
                pd.ds.fail_f=pd.ds.fail_b=pd.ds.zfail_f=pd.ds.zfail_b=1;
                pd.ds.zpass_f=pd.ds.zpass_b=test->stencil==2?1:3;}}
        else pd.cfmt[1]=NVN_FORMAT_RGBA8;
        GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipes[write],&err));
    }
    nvn_gpu_tex images[2]={0};nvn_gpu_view views[2]={0},sample=NULL;
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;
    td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    for(unsigned i=0;i<2;i++){td.d.format=i?NVN_FORMAT_RGBA8:format;GPU(nvn_gpu_tex_create(&td,&images[i],&err));GPU(nvn_gpu_view_create(images[i],NULL,&views[i],&err));}
    GPU(nvn_gpu_view_create(images[0],NULL,&sample,&err));samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));
    unsigned char initial[224];memset(initial,0xa5,sizeof initial);nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(sizeof initial,NVN_GPU_MEM_SHARED,initial,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    nvn_gpu_pass_desc p={0};p.w=p.h=4;p.layers=1;p.ncolor=test->depth?1:2;
    p.color[test->depth?0:1].view=views[1];p.color[test->depth?0:1].load=NVN_GPU_LOAD_CLEAR;p.color[test->depth?0:1].store=NVN_GPU_STORE_STORE;
    nvn_gpu_att *src=test->depth?&p.depth:&p.color[0];src->view=views[0];src->load=NVN_GPU_LOAD_CLEAR;src->store=NVN_GPU_STORE_STORE;
    src->clear.f[0]=.25f;src->clear.f[3]=1;p.clear_depth=.25f;
    if(test->stencil){p.stencil.view=views[0];p.stencil.load=NVN_GPU_LOAD_CLEAR;p.stencil.store=NVN_GPU_STORE_STORE;p.clear_stencil=17;}
    nvn_gpu_pass_begin(cmd,&p);nvn_gpu_pass_end(cmd);
    if(!test->depth)nvn_gpu_copy_tex_buf(cmd,images[0],0,0,NULL,rb,128,16,64);
    src->load=test->load?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD;src->store=test->discard?NVN_GPU_STORE_DONTCARE:NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd,&p);
    nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;
    nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;
    dyn.st_vmask[0]=dyn.st_vmask[1]=255;dyn.st_wmask[0]=dyn.st_wmask[1]=test->stencil==3?0:255;dyn.st_ref[0]=dyn.st_ref[1]=29;
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;unsigned draws=(test->sequence==3||test->sequence==7)?3:test->sequence==5?1:2,writes=0,sampled=0;
    for(unsigned i=0;i<draws;i++){
        if(test->sequence==4&&i==1){nvn_gpu_clear cl={0};cl.color=test->depth?-1:0;cl.color_mask=15;cl.depth=test->depth;cl.depth_value=.5f;cl.value.f[0]=.5f;cl.value.f[3]=1;cl.w=cl.h=4;nvn_gpu_clear_rect(cmd,&cl);}
        if(test->sequence==8&&i==1){nvn_gpu_pass_end(cmd);src->load=NVN_GPU_LOAD_CLEAR;src->clear.f[0]=.5f;nvn_gpu_pass_begin(cmd,&p);}
        int write=test->sequence==1||(test->sequence==2&&i==0)||((test->sequence==3||test->sequence==7)&&i==1);
        int sampling=!(test->sequence==7&&write);sampled+=sampling;
        writes+=sampling&&(write||test->stencil==1);
        nvn_gpu_set_pipe(cmd,pipes[write]);nvn_gpu_set_dyn(cmd,&dyn);
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,sampling?b:NULL,sampling?2:0);nvn_gpu_draw(cmd,&a);
    }
    nvn_gpu_pass_end(cmd);nvn_gpu_copy_tex_buf(cmd,images[1],0,0,NULL,rb,0,16,64);
    if(!test->depth)nvn_gpu_copy_tex_buf(cmd,images[0],0,0,NULL,rb,64,16,64);
    nvn_gpu_samp_release(sampler);nvn_gpu_view_release(sample);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
    int changes=(test->sequence>0&&test->sequence<5)||test->sequence>=7;
    unsigned expected=(changes? (test->depth?191:192):128),bad_pixels=0,source_diff=0;
    const unsigned char *bytes=nvn_gpu_buf_cpu(rb);
    if(read_trace)for(unsigned i=0;i<2;i++){
        char file[1024];int n=snprintf(file,sizeof file,"%s/source-%s.bin",dir,i?"after":"before");REQUIRE(n>0&&(size_t)n<sizeof file);
        FILE *fp=fopen(file,"wb");REQUIRE(fp);REQUIRE(fwrite(bytes+(i?64:128),1,64,fp)==64);REQUIRE(!fclose(fp));
    }
    for(unsigned i=0;i<16;i++)if(bytes[i*4]!=expected||bytes[i*4+1]||bytes[i*4+2]||bytes[i*4+3]!=255)bad_pixels++;
    if(!test->depth){unsigned source=test->sequence==1?192:changes?128:64;
        for(unsigned i=0;i<16;i++){
            if(bytes[64+i*4]!=source||bytes[65+i*4]||bytes[66+i*4]||bytes[67+i*4]!=255)bad_pixels++;
            if(bytes[128+i*4]!=64||bytes[129+i*4]||bytes[130+i*4]||bytes[131+i*4]!=255)bad_pixels++;
            if(memcmp(bytes+64+i*4,bytes+128+i*4,4))source_diff++;
        }
        if(!changes&&source_diff)bad_pixels++;
    }
    for(unsigned i=test->depth?64:192;i<sizeof initial;i++)if(bytes[i]!=0xa5)bad_pixels++;
    unsigned expected_copies=read_mode&&!hide_extension?writes:sampled;
    unsigned bad=bad_pixels||observed_copies!=expected_copies||read_bad_pass||read_bad_write||read_bad_layout||(read_check_layout&&read_next_clear!=1);
    printf("feedback-read case=%s copies=%u expected=%u live=%u unsafe-pass=%u unsafe-write=%u pixel-bad=%u source-diff=%u/16 bad=%u layout-bad=%u next-clear=%u\n",test->name,observed_copies,expected_copies,read_live,read_bad_pass,read_bad_write,bad_pixels,source_diff,bad,read_bad_layout,read_next_clear);
    failures+=!!bad;read_cases++;nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs[0]);nvn_gpu_shader_release(fs[1]);
    GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!observed_early_destroy);
}
int main(int argc,char **argv) {
    if(argc!=6&&argc!=7)return 2;
    int mask_repro=argc==7&&!strcmp(argv[6],"--mask-repro");REQUIRE(argc==6||mask_repro);
    read_trace=mask_repro;
    dq_mode=atoi(argv[2]);opt_mode=atoi(argv[3]);read_mode=atoi(argv[4]);hide_extension=atoi(argv[5]);
    observed_on_begin=read_begin;observed_on_draw=read_draw;
    observed_sample_sets=read_sample_sets;observed_command=read_command;
    struct s2a_vk_device device;char why[256]={0};REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    REQUIRE(enabled_none==!hide_extension);
    uint32_t n=0;REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical,NULL,&n,NULL)==VK_SUCCESS);
    VkExtensionProperties *ext=calloc(n,sizeof *ext);REQUIRE(ext);REQUIRE(__real_vkEnumerateDeviceExtensionProperties(device.physical,NULL,&n,ext)==VK_SUCCESS);
    int advertised=0;for(uint32_t i=0;i<n;i++)advertised|=!strcmp(ext[i].extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);free(ext);REQUIRE(advertised);
    if(mask_repro){const struct read_case test={"color-two-masked-draws",0,0,6,0,0};read_case_run(&device,argv[1],&test);}
    else for(unsigned i=0;i<sizeof read_matrix/sizeof *read_matrix;i++)read_case_run(&device,argv[1],&read_matrix[i]);
    REQUIRE(!s2a_vk_device_destroy(&device,1));
    printf("feedback-read cases=%u failures=%u read=%d hidden=%d extension-enabled=%d\n",read_cases,failures,read_mode,hide_extension,enabled_none);
    return failures?1:0;
}
