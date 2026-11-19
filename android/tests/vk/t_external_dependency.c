// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 真实后端：前一通道清附件，后一通道采样；观察器去掉入向依赖以校准出向依赖。
#define main feedback_original_main
#include "t_feedback.c"
#undef main

static int omit_color_stage, expect_narrow, pass_dep_off, keep_incoming;
static unsigned observed_passes, observed_outputs, mutated_outputs;
static unsigned color_only, color_stage_mismatches;
static unsigned mixed, mixed_stage_mismatches;
static unsigned no_attachment, no_attachment_mismatches;
static unsigned depth_only, depth_only_mismatches;
int __real_nvn_plat_flag(const char *,int);
int __wrap_nvn_plat_flag(const char *name,int d) {
    if(!strcmp(name,"nvk_pass_dep_narrow"))return !pass_dep_off;
    return __real_nvn_plat_flag(name,d);
}
extern __typeof__(vkCreateRenderPass) __real_vkCreateRenderPass;
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice device,const VkRenderPassCreateInfo *ci,
                                                        const VkAllocationCallbacks *allocator,VkRenderPass *pass) {
    REQUIRE(ci->dependencyCount<=2);
    VkSubpassDependency deps[2];uint32_t count=0;
    for(uint32_t i=0;i<ci->dependencyCount;i++) {
        VkSubpassDependency d=ci->pDependencies[i];
        // 删除入向依赖，避免它掩盖被测的出向依赖；模板及实际 pass 一致变异。
        if(d.srcSubpass==VK_SUBPASS_EXTERNAL&&!keep_incoming)continue;
        if(d.dstSubpass==VK_SUBPASS_EXTERNAL) {
            observed_outputs++;
            const VkSubpassDescription *sp=&ci->pSubpasses[0];
            int depth=sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment!=VK_ATTACHMENT_UNUSED;
            unsigned colors=0;for(uint32_t j=0;j<sp->colorAttachmentCount;j++)if(sp->pColorAttachments[j].attachment!=VK_ATTACHMENT_UNUSED)colors++;
            if(colors&&!depth){
                color_only++;
                VkPipelineStageFlags stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkAccessFlags access=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                if(pass_dep_off){stages|=VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;access|=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;}
                if(d.srcStageMask!=stages || d.srcAccessMask!=access)color_stage_mismatches++;
            }
            if(colors&&depth){mixed++;if(d.srcStageMask!=(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)||d.srcAccessMask!=(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))mixed_stage_mismatches++;}
            if(!colors&&!depth){no_attachment++;if(d.srcStageMask!=(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)||d.srcAccessMask!=(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))no_attachment_mismatches++;}
            if(!colors&&depth){depth_only++;if(d.srcStageMask!=(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)||d.srcAccessMask!=(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))depth_only_mismatches++;}
            if(omit_color_stage) {
                d.srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                d.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                mutated_outputs++;
            }
            // ALL_COMMANDS 在 subpass 依赖中等价 ALL_GRAPHICS。
            if(!keep_incoming&&d.dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)d.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        }
        deps[count++]=d;
    }
    VkRenderPassCreateInfo copy=*ci;copy.dependencyCount=count;copy.pDependencies=count?deps:NULL;
    observed_passes++;return __real_vkCreateRenderPass(device,&copy,allocator,pass);
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_vkCreateRenderPass), PFN_vkCreateRenderPass), "vkCreateRenderPass signature");

static unsigned no_sample_sets(void) { return 0; }
static void run_empty(struct s2a_vk_device *device,const char *dir) {
    observer_reset();observed_sample_sets=no_sample_sets;
    nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="empty-pass-dependency";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,"solid_green.frag",NVN_GPU_SH_FS,NULL);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    // 两个空颜色槽，管线模板没有颜色槽；两者均不得误判为颜色附件。
    nvn_gpu_pass_desc p={0};p.ncolor=2;p.w=p.h=4;p.layers=1;nvn_gpu_pass_begin(cmd,&p);
    nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;nvn_gpu_draw(cmd,&a);nvn_gpu_pass_end(cmd);
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));REQUIRE(observed_draws==1);
    nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));observed_sample_sets=NULL;
}

static void run_depth_only(struct s2a_vk_device *device,const char *dir) {
    observer_reset();observed_sample_sets=no_sample_sets;
    nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="depth-only-pass-dependency";GPU(nvn_gpu_open(&od,&err));
    nvn_gpu_shader vs=shader(dir,"fullscreen.vert",NVN_GPU_SH_VS,NULL),fs=shader(dir,"solid_green.frag",NVN_GPU_SH_FS,NULL);
    nvn_gpu_pipe_desc pd={0};pd.poly.mode=NVN_POLYGON_MODE_FILL;pd.color.logic_op=3;pd.prim=4;pd.samples=1;pd.dfmt=NVN_FORMAT_DEPTH32F;pd.ds.dt=pd.ds.dw=1;pd.ds.df=8;
    pd.st[NVN_GPU_SH_VS].sh=vs;pd.st[NVN_GPU_SH_FS].sh=fs;
    nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err));
    nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_DEPTH32F;td.d.width=td.d.height=4;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex=NULL;nvn_gpu_view view=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&view,&err));
    nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
    nvn_gpu_pass_desc p={0};p.depth.view=view;p.depth.load=NVN_GPU_LOAD_CLEAR;p.depth.store=NVN_GPU_STORE_STORE;p.clear_depth=.25f;p.w=p.h=4;p.layers=1;nvn_gpu_pass_begin(cmd,&p);
    nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_dyn dyn={0};dyn.nvp=1;dyn.vp[0][2]=dyn.vp[0][3]=4;dyn.vp[0][5]=1;dyn.has_sc=1;dyn.sc[2]=dyn.sc[3]=4;nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_draw_args a={0};a.prim=4;a.count=3;a.instances=1;nvn_gpu_draw(cmd,&a);nvn_gpu_pass_end(cmd);
    GPU(nvn_gpu_cmd_commit(cmd,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));REQUIRE(observed_draws==1);
    nvn_gpu_view_release(view);nvn_gpu_tex_release(tex);nvn_gpu_shader_release(vs);nvn_gpu_shader_release(fs);GPU(nvn_gpu_close(5000000000ull,&err));observed_sample_sets=NULL;
}

int main(int argc,char **argv) {
    REQUIRE(argc==4);dq_mode=atoi(argv[2]);int mode=atoi(argv[3]);omit_color_stage=mode==1;expect_narrow=mode==2||mode==3||mode==6;pass_dep_off=mode==3;keep_incoming=mode==6;opt_mode=0;copy_mode=1;
    REQUIRE((dq_mode==0||dq_mode==1)&&mode>=0&&mode<=7);
    struct s2a_vk_device device;char why[256];REQUIRE(!s2a_vk_device_create(&device,why,sizeof why));
    if(mode==7)run_depth_only(&device,argv[1]);else if(mode==5)run_empty(&device,argv[1]);else if(mode==4)run_depth(&device,argv[1],NVN_FORMAT_DEPTH32F,12);else run_case(&device,argv[1],0);
    REQUIRE(!s2a_vk_device_destroy(&device,1));
    REQUIRE(observed_passes>=2&&observed_outputs>=2);
    REQUIRE(omit_color_stage?mutated_outputs==observed_outputs:mutated_outputs==0);
    printf("external dq=%d omit_color=%d passes=%u outputs=%u mutated=%u color_only=%u stage_mismatches=%u mixed=%u mixed_mismatches=%u no_attachment=%u no_attachment_mismatches=%u depth_only=%u depth_only_mismatches=%u failures=%u\n",
           dq_mode,omit_color_stage,observed_passes,observed_outputs,mutated_outputs,color_only,color_stage_mismatches,mixed,mixed_stage_mismatches,no_attachment,no_attachment_mismatches,depth_only,depth_only_mismatches,failures);
    return failures||(expect_narrow&&(!color_only||color_stage_mismatches))||(mode==4&&(!mixed||mixed_stage_mismatches))||(mode==5&&(!no_attachment||no_attachment_mismatches))||(mode==7&&(!depth_only||depth_only_mismatches))?1:0;
}
