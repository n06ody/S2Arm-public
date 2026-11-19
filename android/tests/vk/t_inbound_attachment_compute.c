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
static int omit;
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *a,VkRenderPass *p) {
    VkRenderPassCreateInfo copy=*ci; copy.dependencyCount=0; copy.pDependencies=NULL;
    return __real_vkCreateRenderPass(d,&copy,a,p);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass signature");
static PFN_vkCmdEndRenderPass real_end;
static PFN_vkCmdPipelineBarrier real_barrier;
static PFN_vkCmdDispatch real_dispatch;
static unsigned dispatches;
static unsigned between_barriers, removed_full_barriers;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdEndRenderPass(VkCommandBuffer cmd) {
    real_end(cmd);
    if (++ends == 1 && omit != 3) {
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=omit==1?0:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=omit==2?0:VK_ACCESS_SHADER_READ_BIT};
        VkPipelineStageFlags src=omit==1?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkPipelineStageFlags dst=omit==2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        vkCmdPipelineBarrier(cmd,src,dst,0,1,&barrier,0,NULL,0,NULL);
    }
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCmdEndRenderPass), PFN_vkCmdEndRenderPass), "vkCmdEndRenderPass signature");
VKAPI_ATTR void VKAPI_CALL stage_barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
    if(ends==1&&!dispatches){
        between_barriers++;
        if(src==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && nm==1 && nb==0 && ni==0 &&
           m[0].srcAccessMask==VK_ACCESS_MEMORY_WRITE_BIT && m[0].dstAccessMask==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT)){
            removed_full_barriers++;return; // 仅检测器夹具：移除派发前全屏障，隔离测试的精确依赖。
        }
    }
    real_barrier(cmd,src,dst,flags,nm,m,nb,b,ni,im);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&stage_barrier), PFN_vkCmdPipelineBarrier), "vkCmdPipelineBarrier signature");
VKAPI_ATTR void VKAPI_CALL stage_dispatch(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z){dispatches++;real_dispatch(cmd,x,y,z);}
_Static_assert(__builtin_types_compatible_p(__typeof__(&stage_dispatch), PFN_vkCmdDispatch), "vkCmdDispatch signature");
static PFN_vkVoidFunction stage_hook(const char *name) {
    if(!strcmp(name,"vkCmdEndRenderPass"))return (PFN_vkVoidFunction)__wrap_vkCmdEndRenderPass;
    if(!strcmp(name,"vkCmdPipelineBarrier"))return (PFN_vkVoidFunction)stage_barrier;
    if(!strcmp(name,"vkCmdDispatch"))return (PFN_vkVoidFunction)stage_dispatch;
    return NULL;
}
static unsigned no_samples(void){return 0;}
int main(int argc,char **argv){
    REQUIRE(argc==6);dq_mode=atoi(argv[2]);int alias=atoi(argv[3]);omit=atoi(argv[4]);int xseg=atoi(argv[5]);
    REQUIRE((dq_mode==0||dq_mode==1)&&(alias==0||alias==1)&&omit>=0&&omit<=2&&(xseg==0||xseg==1));opt_mode=0;copy_mode=1;
    struct s2a_vk_device device;char why[256];REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    observer_reset();observed_sample_sets=no_samples;
    real_end=(PFN_vkCmdEndRenderPass)__real_vkGetDeviceProcAddr(device.device,"vkCmdEndRenderPass");REQUIRE(real_end);
    real_barrier=(PFN_vkCmdPipelineBarrier)__real_vkGetDeviceProcAddr(device.device,"vkCmdPipelineBarrier");REQUIRE(real_barrier);
    real_dispatch=(PFN_vkCmdDispatch)__real_vkGetDeviceProcAddr(device.device,"vkCmdDispatch");REQUIRE(real_dispatch);observed_command=stage_hook;
    nvn_gpu_open_desc od={0};od.host_device=&device;od.app_label="inbound-attachment-compute";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent[3]={{0}};ent[0].cls=NVN_GPU_RES_TEX;ent[1].cls=NVN_GPU_RES_SAMP;ent[2].cls=NVN_GPU_RES_SSBO;
    for(int i=0;i<3;i++){ent[i].binding=(uint32_t)i;ent[i].access=i==2?NVN_GPU_ACC_W:NVN_GPU_ACC_R;ent[i].count=1;ent[i].stages=1u<<NVN_GPU_SH_CS;}
    nvn_gpu_layout layout={.ent=ent,.nent=3};nvn_gpu_shader cs=shader(argv[1],"inbound_attachment.comp",NVN_GPU_SH_CS,&layout);
    nvn_gpu_stage_desc stage={0};stage.sh=cs;nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_cpipe_get(&stage,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
    nvn_gpu_tex tex=NULL;nvn_gpu_view view=NULL,sample=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&view,&err));
    if(alias)GPU(nvn_gpu_view_create(tex,NULL,&sample,&err));else sample=view;
    samp_par sp={0};nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sp,&sampler,&err));float zero[4]={0};nvn_gpu_buf result=NULL;GPU(nvn_gpu_buf_create(sizeof zero,NVN_GPU_MEM_SHARED,zero,&result,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));nvn_gpu_pass_desc pass={0};pass.ncolor=1;pass.color[0].view=view;pass.color[0].load=NVN_GPU_LOAD_CLEAR;pass.color[0].store=NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[1]=1;pass.color[0].clear.f[3]=1;pass.w=pass.h=4;pass.layers=1;nvn_gpu_pass_begin(cmd,&pass);nvn_gpu_pass_end(cmd);
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=1;pthread_mutex_unlock(&wait_mu);GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));}
    nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_binding b[3]={{0}};b[0].cls=NVN_GPU_RES_TEX;b[0].u.view=sample;b[1].ent=1;b[1].cls=NVN_GPU_RES_SAMP;b[1].u.samp=sampler;b[2].ent=2;b[2].cls=NVN_GPU_RES_SSBO;b[2].u.b.buf=result;b[2].u.b.len=sizeof zero;
    nvn_gpu_bind(cmd,NVN_GPU_SH_CS,b,3);nvn_gpu_dispatch(cmd,pipe,1,1,1,NULL,0);GPU(nvn_gpu_cmd_commit(cmd,xseg?2:1,done,NULL,&err));
    if(xseg){pthread_mutex_lock(&wait_mu);hold_wait=0;pthread_cond_broadcast(&wait_cv);pthread_mutex_unlock(&wait_mu);}GPU(nvn_gpu_fence_wait(xseg?2:1,5000000000ull,&err));
    float value[4];memcpy(value,nvn_gpu_buf_cpu(result),sizeof value);unsigned bad=value[0]!=0||value[1]!=1||value[2]!=0||value[3]!=1;
    REQUIRE(dispatches==1&&ends==1&&removed_full_barriers==1);printf("attachment-compute dq=%d alias=%d omit=%d xseg=%d waits_held=%u removed_full=%u value_bad=%u\n",dq_mode,alias,omit,xseg,held_waits,removed_full_barriers,bad);
    if(alias){nvn_gpu_view_release(sample);}nvn_gpu_view_release(view);nvn_gpu_tex_release(tex);nvn_gpu_samp_release(sampler);nvn_gpu_buf_release(result,NULL,NULL);nvn_gpu_shader_release(cs);
    GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&device,1));return (!omit&&bad)?1:0;
}
