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
static int src_vertex, omit;
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *a,VkRenderPass *p) {
    VkRenderPassCreateInfo copy=*ci; copy.dependencyCount=0; copy.pDependencies=NULL;
    return __real_vkCreateRenderPass(d,&copy,a,p);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass signature");
static PFN_vkCmdEndRenderPass real_end;
static PFN_vkCmdPipelineBarrier real_barrier;
static PFN_vkCmdDrawIndirect real_indirect;
static unsigned indirect_draws;
static unsigned between_barriers;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdEndRenderPass(VkCommandBuffer cmd) {
    real_end(cmd);
    if (++ends == 1 && omit != 3) {
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=omit==1?0:VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask=omit==2?0:VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        VkPipelineStageFlags src=omit==1?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            src_vertex?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        VkPipelineStageFlags dst=omit==2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        vkCmdPipelineBarrier(cmd,src,dst,0,1,&barrier,0,NULL,0,NULL);
    }
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdEndRenderPass), PFN_vkCmdEndRenderPass), "vkCmdEndRenderPass signature");
VKAPI_ATTR void VKAPI_CALL stage_barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
    if(observed_draws==1){between_barriers++;fprintf(stderr,"between-barrier src=%#x dst=%#x mem=%u buf=%u img=%u\n",src,dst,nm,nb,ni);}
    real_barrier(cmd,src,dst,flags,nm,m,nb,b,ni,im);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&stage_barrier), PFN_vkCmdPipelineBarrier), "vkCmdPipelineBarrier signature");
VKAPI_ATTR void VKAPI_CALL stage_indirect(VkCommandBuffer cmd,VkBuffer buffer,VkDeviceSize off,uint32_t n,uint32_t stride){indirect_draws++;real_indirect(cmd,buffer,off,n,stride);}
_Static_assert(__builtin_types_compatible_p(__typeof__(&stage_indirect), PFN_vkCmdDrawIndirect), "vkCmdDrawIndirect signature");
static PFN_vkVoidFunction stage_hook(const char *name) {
    if(!strcmp(name,"vkCmdEndRenderPass"))return (PFN_vkVoidFunction)__wrap_vkCmdEndRenderPass;
    if(!strcmp(name,"vkCmdPipelineBarrier"))return (PFN_vkVoidFunction)stage_barrier;
    if(!strcmp(name,"vkCmdDrawIndirect"))return (PFN_vkVoidFunction)stage_indirect;
    return NULL;
}
static unsigned no_samples(void){return 0;}
int main(int argc,char **argv){
    REQUIRE(argc==6);dq_mode=atoi(argv[2]);src_vertex=atoi(argv[3]);omit=atoi(argv[4]);int xseg=atoi(argv[5]);
    REQUIRE((dq_mode==0||dq_mode==1)&&(src_vertex==0||src_vertex==1)&&omit>=0&&omit<=2&&(xseg==0||xseg==1));opt_mode=0;copy_mode=1;
    struct s2a_vk_device device;char why[256];REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    observer_reset();observed_sample_sets=no_samples;
    real_end=(PFN_vkCmdEndRenderPass)__real_vkGetDeviceProcAddr(device.device,"vkCmdEndRenderPass");REQUIRE(real_end);
    real_barrier=(PFN_vkCmdPipelineBarrier)__real_vkGetDeviceProcAddr(device.device,"vkCmdPipelineBarrier");REQUIRE(real_barrier);
    real_indirect=(PFN_vkCmdDrawIndirect)__real_vkGetDeviceProcAddr(device.device,"vkCmdDrawIndirect");REQUIRE(real_indirect);observed_command=stage_hook;
    nvn_gpu_open_desc od={0};od.host_device=&device;od.app_label="inbound-indirect";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent={0};ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_W;ent.count=1;ent.stages=1u<<(src_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);
    nvn_gpu_layout layout={.ent=&ent,.nent=1};
    nvn_gpu_shader plain_vs=shader(argv[1],"fullscreen.vert",NVN_GPU_SH_VS,NULL);
    nvn_gpu_shader black=shader(argv[1],"inbound_black.frag",NVN_GPU_SH_FS,NULL);
    nvn_gpu_shader green=shader(argv[1],"solid_green.frag",NVN_GPU_SH_FS,NULL);
    nvn_gpu_shader write=shader(argv[1],src_vertex?"inbound_indirect_write.vert":"inbound_indirect_write.frag",src_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    pd.st[NVN_GPU_SH_VS].sh=src_vertex?write:plain_vs;pd.st[NVN_GPU_SH_FS].sh=src_vertex?black:write;
    nvn_gpu_pipe pipes[2]={0};GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipes[0],&err));
    pd.st[NVN_GPU_SH_VS].sh=plain_vs;pd.st[NVN_GPU_SH_FS].sh=green;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipes[1],&err));
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex[2]={0};nvn_gpu_view view[2]={0};for(unsigned i=0;i<2;i++){GPU(nvn_gpu_tex_create(&td,&tex[i],&err));GPU(nvn_gpu_view_create(tex[i],NULL,&view[i],&err));}
    uint32_t zeros[4]={0};nvn_gpu_buf args=NULL,rb=NULL;GPU(nvn_gpu_buf_create(16,NVN_GPU_MEM_SHARED,zeros,&args,&err));GPU(nvn_gpu_buf_create(64,NVN_GPU_MEM_SHARED,NULL,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;
    begin_pass(cmd,view[0],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_set_pipe(cmd,pipes[0]);nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding b={0};b.cls=NVN_GPU_RES_SSBO;b.u.b.buf=args;b.u.b.len=16;nvn_gpu_bind(cmd,src_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS,&b,1);
    nvn_gpu_draw_args draw={0};draw.prim=4;draw.count=3;draw.instances=1;nvn_gpu_draw(cmd,&draw);nvn_gpu_pass_end(cmd);
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=1;pthread_mutex_unlock(&wait_mu);GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));}
    begin_pass(cmd,view[1],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_set_pipe(cmd,pipes[1]);nvn_gpu_set_dyn(cmd,&dyn);
    if(!xseg)nvn_gpu_bind(cmd,src_vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS,NULL,0);
    memset(&draw,0,sizeof draw);draw.prim=4;draw.ind=args;draw.max_draws=1;draw.stride=16;nvn_gpu_draw(cmd,&draw);nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex_buf(cmd,tex[1],0,0,NULL,rb,0,16,64);GPU(nvn_gpu_cmd_commit(cmd,xseg?2:1,done,NULL,&err));
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=0;pthread_cond_broadcast(&wait_cv);pthread_mutex_unlock(&wait_mu);}GPU(nvn_gpu_fence_wait(xseg?2:1,5000000000ull,&err));
    unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<16;i++)if(pixels[i*4]||pixels[i*4+1]!=255||pixels[i*4+2]||pixels[i*4+3]!=255)bad++;
    REQUIRE(observed_draws==1&&indirect_draws==1&&ends==2);
    printf("indirect dq=%d src=%s omit=%d xseg=%d waits_held=%u pixels_bad=%u\n",dq_mode,src_vertex?"VS":"FS",omit,xseg,held_waits,bad);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(view[i]);nvn_gpu_tex_release(tex[i]);}
    nvn_gpu_buf_release(args,NULL,NULL);nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(plain_vs);nvn_gpu_shader_release(black);nvn_gpu_shader_release(green);nvn_gpu_shader_release(write);
    GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&device,1));return (!omit&&bad)?1:0;
}
