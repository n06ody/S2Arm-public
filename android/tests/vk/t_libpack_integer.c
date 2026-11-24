// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#define main backend_fixture_main
#include "t_backend_vk.c"
#undef main
extern int numeric_candidate(const uint32_t *spv, size_t bytes, const uint32_t formats[2]);

int main(int argc, char **argv) {
    if (argc!=3) return 2;
    int negative=!strcmp(argv[2],"withdrawn");
    char why[256],path[1024];
    if (s2a_vk_device_create(&g_dev,why,sizeof why)) { fprintf(stderr,"device: %s\n",why); return 2; }
    g_desc.host_device=&g_dev;
    if (!open_backend()) return 2;
    const uint32_t formats[]={NVN_FORMAT_R11G11B10F,NVN_FORMAT_RGBA8UI,NVN_FORMAT_RGBA8UI,NVN_FORMAT_RG16UI};
    uint32_t sample_fmt[]={formats[2],formats[3]};
    size_t bytes=0;
    snprintf(path,sizeof path,"%s/float.frag.spv",argv[1]);
    void *candidate=read_file(path,&bytes);
    if (!candidate) return 2;
    int accepted=numeric_candidate(candidate,bytes,sample_fmt);
    free(candidate);
    CHECK(accepted==negative,"integer admission: accepted=%d expected=%d",accepted,negative);
    if (accepted<0) return 2;
    const char *selected=accepted ? "float.frag" : "uint.frag";
    void *vm=NULL,*fm=NULL;
    nvn_gpu_shader vs=NULL,fs=NULL;
    nvn_gpu_layout_ent inputs[2];
    for (unsigned i=0;i<2;i++) inputs[i]=(nvn_gpu_layout_ent){.binding=i,.cls=NVN_GPU_RES_TEX_SAMP,
        .access=NVN_GPU_ACC_R,.count=1,.stages=1u<<NVN_GPU_SH_FS};
    nvn_gpu_layout layout={.nent=2,.ent=inputs};
    if (!load_shader_pair(argv[1],"fullscreen.vert",selected,NULL,&layout,&vs,&fs,&vm,&fm)) return 2;
    nvn_gpu_pipe_desc pd={.samples=1,.prim=4,.cfmt={NVN_FORMAT_RGBA8UI}};
    pd.st[0].sh=vs; pd.st[1].sh=fs; pd.chmask.mask=15; pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.color.logic_op=3;
    nvn_gpu_pipe pipe=NULL; nvn_gpu_err err={0};
    if (!OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err))) { fprintf(stderr,"pipe: %s\n",err.why); return 2; }
    samp_par sp={0}; nvn_gpu_samp sampler=NULL;
    if (!OK(nvn_gpu_samp_create(&sp,&sampler,&err))) return 2;
    nvn_gpu_tex targets[5]; nvn_gpu_view views[5];
    nvn_gpu_pass_desc p={.ncolor=4,.w=4,.h=4,.layers=1};
    for (unsigned i=0;i<5;i++) {
        targets[i]=make_tex(4,4,i<4 ? formats[i] : NVN_FORMAT_RGBA8UI,NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED);
        views[i]=make_view(targets[i]);
        if (i<4) {
            p.color[i].view=views[i]; p.color[i].load=NVN_GPU_LOAD_CLEAR; p.color[i].store=NVN_GPU_STORE_STORE;
        }
    }
    p.color[2].clear.u[0]=19; p.color[2].clear.u[2]=213;
    p.color[3].clear.u[0]=0xabc1; p.color[3].clear.u[1]=0x1234;
    nvn_gpu_cmd cmd=begin_cmd();
    nvn_gpu_pass_begin(cmd,&p); nvn_gpu_pass_end(cmd);
    p.ncolor=1; p.color[0].view=views[4];
    nvn_gpu_pass_begin(cmd,&p);
    nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_dyn dyn={.nvp=1,.has_sc=1};
    dyn.vp[0][2]=dyn.vp[0][3]=4; dyn.vp[0][5]=1; dyn.sc[2]=dyn.sc[3]=4;
    nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding bindings[2];
    for (unsigned i=0;i<2;i++) bindings[i]=(nvn_gpu_binding){.ent=i,.cls=NVN_GPU_RES_TEX_SAMP,.u.ts={views[2+i],sampler}};
    nvn_gpu_bind(cmd,NVN_GPU_SH_FS,bindings,2);
    nvn_gpu_draw_args da={.prim=4,.count=3,.instances=1};
    nvn_gpu_draw(cmd,&da); nvn_gpu_pass_end(cmd);
    unsigned bad=0;
    if (negative) nvn_gpu_cmd_abort(cmd);
    else {
        uint8_t sentinel[64]; memset(sentinel,0xee,sizeof sentinel);
        nvn_gpu_buf rb=make_buf(sizeof sentinel,NVN_GPU_MEM_SHARED,sentinel);
        nvn_gpu_copy_tex_buf(cmd,targets[4],0,0,NULL,rb,0,16,64);
        if (!OK(nvn_gpu_cmd_commit(cmd,1,on_done,NULL,&err)) || !OK(nvn_gpu_fence_wait(1,5000000000ull,&err))) {
            fprintf(stderr,"readback: %s\n",err.why); return 2;
        }
        const uint8_t expected[]={19,213,171,52};
        for (int i=0;i<16;i++) bad+=memcmp(buf_cpu(rb)+4*i,expected,4)!=0;
        CHECK(!bad,"integer attachment readback: %u incorrect pixels",bad);
        nvn_gpu_buf_release(rb,NULL,NULL);
    }
    for (unsigned i=0;i<5;i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(targets[i]); }
    nvn_gpu_samp_release(sampler); nvn_gpu_shader_release(fs); nvn_gpu_shader_release(vs); free(fm); free(vm);
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)),"close");
    s2a_vk_device_destroy(&g_dev,1);
    printf("integer attachment arm=%s selected=%s pixels_checked=%u bad=%u failures=%d\n",argv[2],selected,negative?0:16,bad,g_fail);
    return g_fail!=0;
}
