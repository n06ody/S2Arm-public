// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实后端：前一图形通道的片元写存储缓冲，后一通道片元读同一区间。
#define main feedback_original_main
#include "t_feedback.c"
#undef main

static int omit_shader_write;
static unsigned created_passes, incoming, mutated;
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice device,const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *allocator,VkRenderPass *pass) {
    REQUIRE(ci->dependencyCount<=2);VkSubpassDependency deps[2];uint32_t n=0;
    for(uint32_t i=0;i<ci->dependencyCount;i++){
        VkSubpassDependency d=ci->pDependencies[i];
        if(d.dstSubpass==VK_SUBPASS_EXTERNAL)continue; // 模板与实际都去出向依赖，隔离入向依赖
        if(d.srcSubpass==VK_SUBPASS_EXTERNAL){
            incoming++;
            // ALL_COMMANDS 在 subpass 依赖中规范等价 ALL_GRAPHICS。
            if(d.srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)d.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
            if(omit_shader_write){d.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;mutated++;}
        }
        deps[n++]=d;
    }
    VkRenderPassCreateInfo copy=*ci;copy.dependencyCount=n;copy.pDependencies=n?deps:NULL;created_passes++;
    return __real_vkCreateRenderPass(device,&copy,allocator,pass);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass signature");
static unsigned no_samples(void){return 0;}

int main(int argc,char **argv){
    REQUIRE(argc==4);dq_mode=atoi(argv[2]);omit_shader_write=atoi(argv[3]);opt_mode=0;copy_mode=1;
    REQUIRE((dq_mode==0||dq_mode==1)&&(omit_shader_write==0||omit_shader_write==1));
    struct s2a_vk_device device;char why[256];REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    observer_reset();observed_sample_sets=no_samples;
    nvn_gpu_open_desc od={0};od.host_device=&device;od.app_label="inbound-shader-access";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_layout_ent ent={0};ent.cls=NVN_GPU_RES_SSBO;ent.access=NVN_GPU_ACC_RW;ent.count=1;ent.stages=1u<<NVN_GPU_SH_FS;
    nvn_gpu_layout layout={0};layout.ent=&ent;layout.nent=1;
    nvn_gpu_shader vs=shader(argv[1],"fullscreen.vert",NVN_GPU_SH_VS,NULL);
    nvn_gpu_shader write=shader(argv[1],"inbound_storage_write.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_shader read=shader(argv[1],"compute_read_buffer.frag",NVN_GPU_SH_FS,&layout);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.chmask.mask=15;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=write;pd.cfmt[0]=NVN_FORMAT_RGBA8;
    nvn_gpu_pipe producer=NULL,consumer=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&producer,&err));
    pd.st[NVN_GPU_SH_FS].sh=read;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&consumer,&err));
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex[2]={0};nvn_gpu_view view[2]={0};
    for(unsigned i=0;i<2;i++){GPU(nvn_gpu_tex_create(&td,&tex[i],&err));GPU(nvn_gpu_view_create(tex[i],NULL,&view[i],&err));}
    nvn_gpu_buf data=NULL,rb=NULL;GPU(nvn_gpu_buf_create(64,NVN_GPU_MEM_SHARED,NULL,&data,&err));GPU(nvn_gpu_buf_create(64,NVN_GPU_MEM_SHARED,NULL,&rb,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;
    nvn_gpu_binding b={0};b.cls=NVN_GPU_RES_SSBO;b.u.b.buf=data;b.u.b.len=64;
    nvn_gpu_draw_args draw={0};draw.prim=4;draw.count=3;draw.instances=1;
    for(unsigned i=0;i<2;i++){
        begin_pass(cmd,view[i],NVN_GPU_LOAD_CLEAR,4);nvn_gpu_set_pipe(cmd,i?consumer:producer);nvn_gpu_set_dyn(cmd,&dyn);
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&b,1);nvn_gpu_draw(cmd,&draw);nvn_gpu_pass_end(cmd);
    }
    nvn_gpu_copy_tex_buf(cmd,tex[1],0,0,NULL,rb,0,16,64);
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
    unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<16;i++)if(pixels[i*4]||pixels[i*4+1]!=255||pixels[i*4+2]||pixels[i*4+3]!=255)bad++;
    REQUIRE(observed_draws==2&&created_passes>=3&&incoming>=3);REQUIRE(omit_shader_write?mutated==incoming:mutated==0);
    printf("inbound dq=%d omit_shader=%d passes=%u incoming=%u mutated=%u pixels_bad=%u\n",dq_mode,omit_shader_write,created_passes,incoming,mutated,bad);
    for(unsigned i=0;i<2;i++){nvn_gpu_view_release(view[i]);nvn_gpu_tex_release(tex[i]);}
    nvn_gpu_buf_release(data,NULL,NULL);nvn_gpu_buf_release(rb,NULL,NULL);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(write);nvn_gpu_shader_release(read);
    GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&device,1));return bad?1:0;
}
