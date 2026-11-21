// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
/*
#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform sampler2D dep;
layout(set=0,binding=1) uniform usampler2D st;
layout(set=0,binding=2,std430) buffer Out { uint values[]; } o;
void main(){for(int y=0;y<4;y++)for(int x=0;x<4;x++){int i=2*(y*4+x);o.values[i]=floatBitsToUint(texelFetch(dep,ivec2(x,y),0).r);o.values[i+1]=texelFetch(st,ivec2(x,y),0).r;}}
*/
static const uint32_t depth_blit_spv[] = {
    0x07230203u,0x00010300u,0x000d000bu,0x00000051u,0x00000000u,0x00020011u,0x00000001u,0x0006000bu,
    0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,0x00000000u,0x0003000eu,0x00000000u,0x00000001u,
    0x0005000fu,0x00000005u,0x00000004u,0x6e69616du,0x00000000u,0x00060010u,0x00000004u,0x00000011u,
    0x00000001u,0x00000001u,0x00000001u,0x00030003u,0x00000002u,0x000001c2u,0x000a0004u,0x475f4c47u,
    0x4c474f4fu,0x70635f45u,0x74735f70u,0x5f656c79u,0x656e696cu,0x7269645fu,0x69746365u,0x00006576u,
    0x00080004u,0x475f4c47u,0x4c474f4fu,0x6e695f45u,0x64756c63u,0x69645f65u,0x74636572u,0x00657669u,
    0x00040005u,0x00000004u,0x6e69616du,0x00000000u,0x00030005u,0x00000008u,0x00000079u,0x00030005u,
    0x00000013u,0x00000078u,0x00030005u,0x0000001bu,0x00000069u,0x00030005u,0x00000024u,0x0074754fu,
    0x00050006u,0x00000024u,0x00000000u,0x756c6176u,0x00007365u,0x00030005u,0x00000026u,0x0000006fu,
    0x00030005u,0x0000002cu,0x00706564u,0x00030005u,0x00000040u,0x00007473u,0x00040047u,0x00000023u,
    0x00000006u,0x00000004u,0x00030047u,0x00000024u,0x00000002u,0x00050048u,0x00000024u,0x00000000u,
    0x00000023u,0x00000000u,0x00040047u,0x00000026u,0x00000021u,0x00000002u,0x00040047u,0x00000026u,
    0x00000022u,0x00000000u,0x00040047u,0x0000002cu,0x00000021u,0x00000000u,0x00040047u,0x0000002cu,
    0x00000022u,0x00000000u,0x00040047u,0x00000040u,0x00000021u,0x00000001u,0x00040047u,0x00000040u,
    0x00000022u,0x00000000u,0x00040047u,0x00000050u,0x0000000bu,0x00000019u,0x00020013u,0x00000002u,
    0x00030021u,0x00000003u,0x00000002u,0x00040015u,0x00000006u,0x00000020u,0x00000001u,0x00040020u,
    0x00000007u,0x00000007u,0x00000006u,0x0004002bu,0x00000006u,0x00000009u,0x00000000u,0x0004002bu,
    0x00000006u,0x00000010u,0x00000004u,0x00020014u,0x00000011u,0x0004002bu,0x00000006u,0x0000001cu,
    0x00000002u,0x00040015u,0x00000022u,0x00000020u,0x00000000u,0x0003001du,0x00000023u,0x00000022u,
    0x0003001eu,0x00000024u,0x00000023u,0x00040020u,0x00000025u,0x0000000cu,0x00000024u,0x0004003bu,
    0x00000025u,0x00000026u,0x0000000cu,0x00030016u,0x00000028u,0x00000020u,0x00090019u,0x00000029u,
    0x00000028u,0x00000001u,0x00000000u,0x00000000u,0x00000000u,0x00000001u,0x00000000u,0x0003001bu,
    0x0000002au,0x00000029u,0x00040020u,0x0000002bu,0x00000000u,0x0000002au,0x0004003bu,0x0000002bu,
    0x0000002cu,0x00000000u,0x00040017u,0x00000030u,0x00000006u,0x00000002u,0x00040017u,0x00000033u,
    0x00000028u,0x00000004u,0x0004002bu,0x00000022u,0x00000035u,0x00000000u,0x00040020u,0x00000038u,
    0x0000000cu,0x00000022u,0x0004002bu,0x00000006u,0x0000003bu,0x00000001u,0x00090019u,0x0000003du,
    0x00000022u,0x00000001u,0x00000000u,0x00000000u,0x00000000u,0x00000001u,0x00000000u,0x0003001bu,
    0x0000003eu,0x0000003du,0x00040020u,0x0000003fu,0x00000000u,0x0000003eu,0x0004003bu,0x0000003fu,
    0x00000040u,0x00000000u,0x00040017u,0x00000046u,0x00000022u,0x00000004u,0x00040017u,0x0000004eu,
    0x00000022u,0x00000003u,0x0004002bu,0x00000022u,0x0000004fu,0x00000001u,0x0006002cu,0x0000004eu,
    0x00000050u,0x0000004fu,0x0000004fu,0x0000004fu,0x00050036u,0x00000002u,0x00000004u,0x00000000u,
    0x00000003u,0x000200f8u,0x00000005u,0x0004003bu,0x00000007u,0x00000008u,0x00000007u,0x0004003bu,
    0x00000007u,0x00000013u,0x00000007u,0x0004003bu,0x00000007u,0x0000001bu,0x00000007u,0x0003003eu,
    0x00000008u,0x00000009u,0x000200f9u,0x0000000au,0x000200f8u,0x0000000au,0x000400f6u,0x0000000cu,
    0x0000000du,0x00000000u,0x000200f9u,0x0000000eu,0x000200f8u,0x0000000eu,0x0004003du,0x00000006u,
    0x0000000fu,0x00000008u,0x000500b1u,0x00000011u,0x00000012u,0x0000000fu,0x00000010u,0x000400fau,
    0x00000012u,0x0000000bu,0x0000000cu,0x000200f8u,0x0000000bu,0x0003003eu,0x00000013u,0x00000009u,
    0x000200f9u,0x00000014u,0x000200f8u,0x00000014u,0x000400f6u,0x00000016u,0x00000017u,0x00000000u,
    0x000200f9u,0x00000018u,0x000200f8u,0x00000018u,0x0004003du,0x00000006u,0x00000019u,0x00000013u,
    0x000500b1u,0x00000011u,0x0000001au,0x00000019u,0x00000010u,0x000400fau,0x0000001au,0x00000015u,
    0x00000016u,0x000200f8u,0x00000015u,0x0004003du,0x00000006u,0x0000001du,0x00000008u,0x00050084u,
    0x00000006u,0x0000001eu,0x0000001du,0x00000010u,0x0004003du,0x00000006u,0x0000001fu,0x00000013u,
    0x00050080u,0x00000006u,0x00000020u,0x0000001eu,0x0000001fu,0x00050084u,0x00000006u,0x00000021u,
    0x0000001cu,0x00000020u,0x0003003eu,0x0000001bu,0x00000021u,0x0004003du,0x00000006u,0x00000027u,
    0x0000001bu,0x0004003du,0x0000002au,0x0000002du,0x0000002cu,0x0004003du,0x00000006u,0x0000002eu,
    0x00000013u,0x0004003du,0x00000006u,0x0000002fu,0x00000008u,0x00050050u,0x00000030u,0x00000031u,
    0x0000002eu,0x0000002fu,0x00040064u,0x00000029u,0x00000032u,0x0000002du,0x0007005fu,0x00000033u,
    0x00000034u,0x00000032u,0x00000031u,0x00000002u,0x00000009u,0x00050051u,0x00000028u,0x00000036u,
    0x00000034u,0x00000000u,0x0004007cu,0x00000022u,0x00000037u,0x00000036u,0x00060041u,0x00000038u,
    0x00000039u,0x00000026u,0x00000009u,0x00000027u,0x0003003eu,0x00000039u,0x00000037u,0x0004003du,
    0x00000006u,0x0000003au,0x0000001bu,0x00050080u,0x00000006u,0x0000003cu,0x0000003au,0x0000003bu,
    0x0004003du,0x0000003eu,0x00000041u,0x00000040u,0x0004003du,0x00000006u,0x00000042u,0x00000013u,
    0x0004003du,0x00000006u,0x00000043u,0x00000008u,0x00050050u,0x00000030u,0x00000044u,0x00000042u,
    0x00000043u,0x00040064u,0x0000003du,0x00000045u,0x00000041u,0x0007005fu,0x00000046u,0x00000047u,
    0x00000045u,0x00000044u,0x00000002u,0x00000009u,0x00050051u,0x00000022u,0x00000048u,0x00000047u,
    0x00000000u,0x00060041u,0x00000038u,0x00000049u,0x00000026u,0x00000009u,0x0000003cu,0x0003003eu,
    0x00000049u,0x00000048u,0x000200f9u,0x00000017u,0x000200f8u,0x00000017u,0x0004003du,0x00000006u,
    0x0000004au,0x00000013u,0x00050080u,0x00000006u,0x0000004bu,0x0000004au,0x0000003bu,0x0003003eu,
    0x00000013u,0x0000004bu,0x000200f9u,0x00000014u,0x000200f8u,0x00000016u,0x000200f9u,0x0000000du,
    0x000200f8u,0x0000000du,0x0004003du,0x00000006u,0x0000004cu,0x00000008u,0x00050080u,0x00000006u,
    0x0000004du,0x0000004cu,0x0000003bu,0x0003003eu,0x00000008u,0x0000004du,0x000200f9u,0x0000000au,
    0x000200f8u,0x0000000cu,0x000100fdu,0x00010038u,
};

// Original constant-buffer fragment shader for physical descriptor restoration.
/*
#version 450
layout(set=0,binding=0,std140) uniform Color { vec4 value; } color;
layout(location=0) out vec4 outputColor;
void main() { outputColor=color.value; }
*/
static const uint32_t depth_state_frag_spv[] = {
    0x07230203u, 0x00010300u, 0x000d000bu, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
    0x00000007u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00030047u, 0x0000000au, 0x00000002u,
    0x00050048u, 0x0000000au, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x0000000cu, 0x00000021u,
    0x00000000u, 0x00040047u, 0x0000000cu, 0x00000022u, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u,
    0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
    0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u,
    0x00000003u, 0x0003001eu, 0x0000000au, 0x00000007u, 0x00040020u, 0x0000000bu, 0x00000002u, 0x0000000au,
    0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000002u, 0x00040015u, 0x0000000du, 0x00000020u, 0x00000001u,
    0x0004002bu, 0x0000000du, 0x0000000eu, 0x00000000u, 0x00040020u, 0x0000000fu, 0x00000002u, 0x00000007u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x00050041u,
    0x0000000fu, 0x00000010u, 0x0000000cu, 0x0000000eu, 0x0004003du, 0x00000007u, 0x00000011u, 0x00000010u,
    0x0003003eu, 0x00000009u, 0x00000011u, 0x000100fdu, 0x00010038u,
};

static void test_depth_blit(unsigned ab_seconds) {
    const int alternate = ab_seconds != 0;
    const unsigned cases = alternate ? 8 : 4;
    for (unsigned i=0;i<cases;i++) {
        const unsigned round = alternate ? i/2 : i;
        if (alternate) {
            nvn_vk_nvnbar_submit((1ull + (uint64_t)ab_seconds * i) * 1000000000ull);
            CHECK(nvn_vk_nvnbar_state() == (((uint64_t)(i+1) << 1) | (i & 1u)));
        }
        int subresource = round >= 2; const unsigned st[4] = {0, 255, 85, 170};
        nvn_gpu_tex_desc td={0};td.d.target=subresource ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D;
        td.d.format=(round & 1) ? NVN_FORMAT_DEPTH32F_STENCIL8 : NVN_FORMAT_DEPTH24_STENCIL8;
        td.d.width=td.d.height=subresource ? 8 : 4;td.d.depth=subresource ? 3 : 1;td.d.samples=1;td.d.levels=subresource ? 2 : 1;
        td.usage=NVN_GPU_TEX_SAMPLED|NVN_GPU_TEX_RENDER;
        nvn_gpu_tex src=NULL,dst=NULL;GPU(nvn_gpu_tex_create(&td,&src,&err));GPU(nvn_gpu_tex_create(&td,&dst,&err));
        view_t selected={0};selected.set_target=1;selected.target=NVN_TEXTURE_TARGET_2D;selected.set_levels=1;selected.base_level=subresource ? 1 : 0;selected.nlevels=1;selected.set_layers=1;selected.min_layer=subresource ? 2 : 0;selected.nlayers=1;
        nvn_gpu_view sv=NULL,dv=NULL,stencil=NULL;GPU(nvn_gpu_view_create(src,&selected,&sv,&err));GPU(nvn_gpu_view_create(dst,&selected,&dv,&err));
        view_t vd=selected;vd.set_dsmode=1;vd.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
        GPU(nvn_gpu_view_create(dst,&vd,&stencil,&err));
        nvn_gpu_cmd cmd=NULL;GPU(nvn_gpu_cmd_begin(NULL,&cmd,&err));
        nvn_gpu_pass_desc pass={0};pass.w=pass.h=4;pass.layers=1;
        pass.depth.view=pass.stencil.view=sv;pass.depth.load=pass.stencil.load=NVN_GPU_LOAD_CLEAR;
        pass.depth.store=pass.stencil.store=NVN_GPU_STORE_STORE;pass.clear_depth=.125f;pass.clear_stencil=st[0];
        nvn_gpu_pass_begin(cmd,&pass);
        for (unsigned q=1;q<4;q++) {
            nvn_gpu_clear clear={0};clear.color=-1;clear.depth=clear.stencil=1;clear.stencil_mask=255;
            clear.depth_value=.125f+.25f*q;clear.stencil_value=st[q];
            clear.x=2*(q%2);clear.y=2*(q/2);clear.w=clear.h=2;nvn_gpu_clear_rect(cmd,&clear);
        }
        nvn_gpu_pass_end(cmd);
        pass.depth.view=pass.stencil.view=dv;pass.clear_depth=.9375f;pass.clear_stencil=0xa5;
        nvn_gpu_pass_begin(cmd,&pass);nvn_gpu_pass_end(cmd);
        NVNcopyRegion a={0,0,0,4,4,1},b={1,1,0,2,2,1};nvn_gpu_blit(cmd,sv,&a,dv,&b,0);
        nvn_gpu_layout_ent entries[3]={
            {.set=0,.binding=0,.count=1,.stages=1u<<NVN_GPU_SH_CS,.cls=NVN_GPU_RES_TEX_SAMP},
            {.set=0,.binding=1,.count=1,.stages=1u<<NVN_GPU_SH_CS,.cls=NVN_GPU_RES_TEX_SAMP},
            {.set=0,.binding=2,.count=1,.stages=1u<<NVN_GPU_SH_CS,.cls=NVN_GPU_RES_SSBO,.access=NVN_GPU_ACC_RW}};
        nvn_gpu_layout layout={.ent=entries,.nent=3};
        nvn_gpu_shader_src shader_src={.stage=NVN_GPU_SH_CS,.kind=NVN_GPU_BLOB_SPIRV,.entry="main",.layout=&layout,.blob=depth_blit_spv,.bytes=sizeof depth_blit_spv};
        nvn_gpu_shader shader=NULL;GPU(nvn_gpu_shader_load(&shader_src,&shader,&err));
        nvn_gpu_stage_desc stage={.sh=shader};nvn_gpu_pipe pipe=NULL;GPU(nvn_gpu_cpipe_get(&stage,NVN_GPU_PIPE_SYNC,&pipe,&err));
        samp_par sd={0};sd.minf=NVN_MIN_FILTER_NEAREST;sd.magf=NVN_MAG_FILTER_NEAREST;sd.ws=sd.wt=sd.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE;sd.aniso=1;
        nvn_gpu_samp sampler=NULL;GPU(nvn_gpu_samp_create(&sd,&sampler,&err));
        uint32_t initial[36];memset(initial,0xcc,sizeof initial);nvn_gpu_buf output=buffer(sizeof initial,initial);
        nvn_gpu_binding bindings[3]={
            {.ent=0,.cls=NVN_GPU_RES_TEX_SAMP,.u.ts={dv,sampler}},
            {.ent=1,.cls=NVN_GPU_RES_TEX_SAMP,.u.ts={stencil,sampler}},
            {.ent=2,.cls=NVN_GPU_RES_SSBO,.u.b={output,0,sizeof initial}}};
        nvn_gpu_set_pipe(cmd,pipe);nvn_gpu_bind(cmd,NVN_GPU_SH_CS,bindings,3);nvn_gpu_dispatch(cmd,pipe,1,1,1,NULL,0);
        GPU(nvn_gpu_cmd_commit(cmd,200+i,done,NULL,&err));GPU(nvn_gpu_fence_wait(200+i,5000000000ull,&err));
        const uint32_t *actual=(const uint32_t *)nvn_gpu_buf_cpu(output);CHECK(actual);
        for (unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
            int inside=x>=1&&x<3&&y>=1&&y<3;unsigned q=inside ? (y-1)*2+x-1 : 0;
            float expected=inside ? .125f+.25f*q : .9375f,got;memcpy(&got,actual+2*(y*4+x),4);
            CHECK(fabsf(got-expected)<.000001f);CHECK(actual[2*(y*4+x)+1]==(inside ? st[q] : 0xa5));
        }
        for (unsigned i=32;i<36;i++)CHECK(actual[i]==0xccccccccu);
        nvn_gpu_buf_release(output,NULL,NULL);nvn_gpu_samp_release(sampler);nvn_gpu_shader_release(shader);
        nvn_gpu_view_release(stencil);nvn_gpu_view_release(dv);nvn_gpu_view_release(sv);nvn_gpu_tex_release(dst);nvn_gpu_tex_release(src);
    }
    printf("PASS: depth-stencil nearest blit, 2 formats x 2 subresources x %u modes, %u depth + %u stencil samples (all 8 bits), %u sentinel bytes\n",
           alternate ? 2u : 1u, 16 * cases, 16 * cases, 16 * cases);
}
