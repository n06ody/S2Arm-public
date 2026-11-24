// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Exercise the actual module reflection and admission with two independently
// supplied NVN view formats. Only the fixture supplies the candidate modules.
#ifndef NVK_NUMERIC_PRODUCER
#define NVK_NUMERIC_PRODUCER "../../native/gfx_plat/nvk_shader.c"
#endif
#include NVK_NUMERIC_PRODUCER

int numeric_candidate(const uint32_t *spv, size_t bytes, const uint32_t formats[2]) {
    struct entry candidate={0};
    struct facts facts={0};
    struct s2a_nvn_texture_fact tex[2];
    struct rp_prog *p=&candidate.prog;
    p->lib_sourced=1;
    p->nent[NVN_GPU_SH_FS]=2;
    for (unsigned i=0;i<2;i++) {
        if (s2a_nvn_texture_fact(formats[i],NVN_TEXTURE_TARGET_2D,1,&tex[i],NULL,0)) return -1;
        facts.tex[i]=(struct s2a_compile_texture_src){.stage="fs",.cbuf_slot=2,.handle=90+2*i,.format=tex[i].format};
        nvn_gpu_layout_ent *e=&p->ent[NVN_GPU_SH_FS][i];
        *e=(nvn_gpu_layout_ent){.count=1,.binding=i,.src_kind=NVN_RB_TEX,.src_reg=128+i};
        sampled_decl(spv,bytes,0,i,&p->dtt[NVN_GPU_SH_FS][i],&p->dcls[NVN_GPU_SH_FS][i],NULL);
        if (!p->dtt[NVN_GPU_SH_FS][i]) return -1;
    }
    facts.ntex=2;
    return lib_sampled_numeric_ok(&candidate,&facts,NULL);
}
