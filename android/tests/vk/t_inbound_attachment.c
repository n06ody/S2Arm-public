// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Two passes, no external dependencies: calibrate stage-specific barriers.
#define main feedback_original_main
#define __wrap_vkWaitForFences observed_original_wait
#include "t_feedback.c"
#undef __wrap_vkWaitForFences
#undef main

static pthread_mutex_t wait_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cv=PTHREAD_COND_INITIALIZER;
static int hold_wait;
static unsigned held_waits;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
    pthread_mutex_lock(&wait_mu);
    while(hold_wait){held_waits++;pthread_cond_wait(&wait_cv,&wait_mu);}
    pthread_mutex_unlock(&wait_mu);
    return observed_original_wait(d,n,f,all,timeout);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkWaitForFences), PFN_vkWaitForFences), "vkWaitForFences signature");

static unsigned ends;
static int dst_vertex, omit, depth;
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *a,VkRenderPass *p) {
    VkRenderPassCreateInfo copy=*ci; copy.dependencyCount=0; copy.pDependencies=NULL;
    return __real_vkCreateRenderPass(d,&copy,a,p);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass signature");
static PFN_vkCmdEndRenderPass real_end;
static PFN_vkCmdPipelineBarrier real_barrier;
static unsigned between_barriers;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdEndRenderPass(VkCommandBuffer cmd) {
    real_end(cmd);
    if (++ends == 1 && omit != 3) {
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=omit==1?0:depth?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=omit==2?0:VK_ACCESS_SHADER_READ_BIT};
        VkPipelineStageFlags src=omit==1?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            depth?(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT):VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkPipelineStageFlags dst=omit==2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
            dst_vertex?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        vkCmdPipelineBarrier(cmd,src,dst,0,1,&barrier,0,NULL,0,NULL);
    }
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdEndRenderPass), PFN_vkCmdEndRenderPass), "vkCmdEndRenderPass signature");
VKAPI_ATTR void VKAPI_CALL stage_barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
    if(observed_draws==1){between_barriers++;fprintf(stderr,"between-barrier src=%#x dst=%#x mem=%u buf=%u img=%u\n",src,dst,nm,nb,ni);}
    real_barrier(cmd,src,dst,flags,nm,m,nb,b,ni,im);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&stage_barrier), PFN_vkCmdPipelineBarrier), "vkCmdPipelineBarrier signature");
static PFN_vkVoidFunction stage_hook(const char *name) {
    if(!strcmp(name,"vkCmdEndRenderPass"))return (PFN_vkVoidFunction)__wrap_vkCmdEndRenderPass;
    if(!strcmp(name,"vkCmdPipelineBarrier"))return (PFN_vkVoidFunction)stage_barrier;
    return NULL;
}
static unsigned no_samples(void){return 0;}
int main(int argc,char **argv){
    REQUIRE(argc==8);dq_mode=atoi(argv[2]);dst_vertex=atoi(argv[3]);omit=atoi(argv[4]);int alias=atoi(argv[5]);int xseg=atoi(argv[6]);depth=atoi(argv[7]);
    REQUIRE((dq_mode==0||dq_mode==1)&&(dst_vertex==0||dst_vertex==1)&&omit>=0&&omit<=2&&(alias==0||alias==1)&&(xseg==0||xseg==1)&&(depth==0||depth==1));
    opt_mode=0;copy_mode=1;
    struct s2a_vk_device device;char why[256];REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    observer_reset();observed_sample_sets=no_samples;
    real_end=(PFN_vkCmdEndRenderPass)__real_vkGetDeviceProcAddr(device.device,"vkCmdEndRenderPass");REQUIRE(real_end);
    real_barrier=(PFN_vkCmdPipelineBarrier)__real_vkGetDeviceProcAddr(device.device,"vkCmdPipelineBarrier");REQUIRE(real_barrier);observed_command=stage_hook;
    nvn_gpu_open_desc od={0};od.host_device=&device;od.app_label="inbound-attachment";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent[2]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[1].binding=1;
    for(int i=0;i<2;i++){ent[i].access=NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<(dst_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);}
    nvn_gpu_layout layout={.ent=ent,.nent=2};
    nvn_gpu_shader plain_vs=shader(argv[1],"fullscreen.vert",NVN_GPU_SH_VS,NULL);
    nvn_gpu_shader varying=shader(argv[1],"inbound_varying.frag",NVN_GPU_SH_FS,NULL);
    nvn_gpu_shader read=shader(argv[1],depth?(dst_vertex?"inbound_depth.vert":"inbound_depth.frag"):(dst_vertex?"inbound_attachment.vert":"inbound_attachment.frag"),dst_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    pd.st[NVN_GPU_SH_VS].sh=dst_vertex?read:plain_vs;pd.st[NVN_GPU_SH_FS].sh=dst_vertex?varying:read;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex tex[2]={0};nvn_gpu_view view[2]={0},sample=NULL;
    for(unsigned i=0;i<2;i++){td.d.format=depth&&i==0?NVN_FORMAT_DEPTH32F:NVN_FORMAT_RGBA8;GPU(nvn_gpu_tex_create(&td,&tex[i],&err));GPU(nvn_gpu_view_create(tex[i],NULL,&view[i],&err));}
    if(alias)GPU(nvn_gpu_view_create(tex[0],NULL,&sample,&err));else sample=view[0];
    samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));
    nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(64,NVN_GPU_MEM_SHARED,NULL,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    nvn_gpu_pass_desc pass={0};pass.ncolor=1;pass.color[0].view=view[0];pass.color[0].load=NVN_GPU_LOAD_CLEAR;pass.color[0].store=NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[1]=1;pass.color[0].clear.f[3]=1;pass.w=pass.h=4;pass.layers=1;
    if(depth){memset(&pass,0,sizeof pass);pass.depth.view=view[0];pass.depth.load=NVN_GPU_LOAD_CLEAR;pass.depth.store=NVN_GPU_STORE_STORE;pass.clear_depth=.5f;pass.w=pass.h=4;pass.layers=1;}
    nvn_gpu_pass_begin(cmd,&pass);nvn_gpu_pass_end(cmd);
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=1;pthread_mutex_unlock(&wait_mu);
        GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));}
    begin_pass(cmd,view[1],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding b[2]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;
    nvn_gpu_bind(cmd,dst_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS,b,2);
    nvn_gpu_draw_args draw={0};draw.prim=4;draw.count=3;draw.instances=1;nvn_gpu_draw(cmd,&draw);nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd,tex[1],0,0,NULL,rb,0,16,64);GPU(nvn_gpu_cmd_commit(cmd,xseg?2:1,done,NULL,&err));
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=0;pthread_cond_broadcast(&wait_cv);pthread_mutex_unlock(&wait_mu);}
    GPU(nvn_gpu_fence_wait(xseg?2:1,5000000000ull,&err));
    unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<16;i++)if(pixels[i*4]||pixels[i*4+1]!=255||pixels[i*4+2]||pixels[i*4+3]!=255)bad++;
    REQUIRE(observed_draws==1&&ends==2);
    printf("attachment dq=%d dst=%s omit=%d alias=%d xseg=%d depth=%d waits_held=%u passes=%u pixels_bad=%u\n",dq_mode,dst_vertex?"VS":"FS",omit,alias,xseg,depth,held_waits,ends,bad);
    if(alias) { nvn_gpu_view_release(sample); }
    nvn_gpu_samp_release(sampler);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(view[i]);nvn_gpu_tex_release(tex[i]);}
    nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(plain_vs);nvn_gpu_shader_release(varying);nvn_gpu_shader_release(read);
    GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&device,1));return (!omit&&bad)?1:0;
}
