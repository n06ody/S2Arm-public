// SPDX-License-Identifier: Apache-2.0
// Real replay: same-submission dirty mirror re-upload is recorded in the main segment.
#define main compute_read_original_main
#include "t_replay_compute_read.c"
#undef main
int main(int argc,char **argv){
 encoding_thread=1;REQUIRE(argc==3);int vertex=!strcmp(argv[2],"VS");REQUIRE(vertex||!strcmp(argv[2],"FS"));
 normalize_dependencies=1;track_all_draws=0;REQUIRE(getenv("S2A_INBOUND_MAIN_ONLY"));
 struct s2a_vk_device d;char why[256];REQUIRE(!s2a_vk_device_create(&d,why,sizeof why));
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-inbound-main-transfer";GPU(nvn_gpu_open(&od,&err));
 memset(guest,0,sizeof guest);const uint32_t args[4]={3,1,0,0};memcpy(guest,args,sizeof args);
 const float init[16]={-1,-1,0,1,3,-1,0,1,-1,3,0,1,1,0,0,1};memcpy(guest+256,init,sizeof init);
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 int st=vertex?0:1;graphics.nent[st]=1;nvn_gpu_layout_ent *e=&graphics.ent[st][0];
 e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;e->src_reg=0;e->src_reg2=UINT16_MAX;e->access=NVN_GPU_ACC_R;e->count=1;e->stages=1u<<(vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);
 nvn_gpu_layout layout={.ent=e,.nent=1};
 graphics.sh[0]=shader(argv[1],vertex?"compute_read_buffer.vert":"fullscreen.vert",NVN_GPU_SH_VS,vertex?&layout:NULL);
 graphics.sh[1]=shader(argv[1],vertex?"solid_green.frag":"compute_read_buffer.frag",NVN_GPU_SH_FS,vertex?NULL:&layout);graphics.color_out_mask=15;
 rp_S.ssbo[st][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[st][0].size=64;
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL,mirror=NULL;uint64_t off=0;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,320,&mirror,&off));
 pass(active,1);rp_pass_flush(active);
 const float green[4]={0,1,0,1};memcpy(guest+304,green,sizeof green);untracked=1;
 p_draw draw={0};draw.prim=4;draw.ind=(uint64_t)(uintptr_t)guest;rp_draw(active,&draw,NULL,&draw);
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);REQUIRE(draws==1&&skips==0&&!nvn_unimpl_kinds());
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);
 GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=255||p[2]||p[3]!=255)bad++;}
 REQUIRE(ndraw==1&&inbound_transfers>=1);const char *inject=getenv("S2A_INBOUND_INJECT");int mode=inject?atoi(inject):0;
 REQUIRE(inbound_injected==(mode?inbound_transfers:0));
 printf("INBOUND_TRANSFER dq=%d stage=%s inject=%d transfers=%u injected=%u pixels_bad=%u\n",dq_mode,argv[2],mode,inbound_transfers,inbound_injected,bad);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(graphics.sh[0]);nvn_gpu_shader_release(graphics.sh[1]);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);
 GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
