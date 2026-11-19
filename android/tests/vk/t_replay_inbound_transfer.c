// SPDX-License-Identifier: Apache-2.0
// Real replay: same-submission dirty mirror re-upload is recorded in the main segment.
#define main compute_read_original_main
#include "t_replay_compute_read.c"
#undef main
int main(int argc,char **argv){
 encoding_thread=1;const char *inject=getenv("S2A_INBOUND_INJECT");int mode=inject?atoi(inject):0;REQUIRE(argc==3);int vertex=!strcmp(argv[2],"VS");REQUIRE(vertex||!strcmp(argv[2],"FS"));
 normalize_dependencies=1;track_all_draws=mode==11 || mode==12;REQUIRE(getenv("S2A_INBOUND_MAIN_ONLY"));
 struct s2a_vk_device d;char why[256];REQUIRE(!s2a_vk_device_create(&d,why,sizeof why));
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-inbound-main-transfer";GPU(nvn_gpu_open(&od,&err));
 if(mode>=13 && mode<=16){
  REQUIRE(d.timeline_semaphore_enabled);VkSemaphoreTypeCreateInfoKHR type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE_KHR,.initialValue=0};
  VkSemaphoreCreateInfo info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};REQUIRE(vkCreateSemaphore(d.device,&info,NULL,&inbound_gate)==VK_SUCCESS);
 }
 memset(guest,0,sizeof guest);const uint32_t args[4]={3,1,0,0};memcpy(guest,args,sizeof args);
 const float init[16]={-1,-1,0,1,3,-1,0,1,-1,3,0,1,1,0,0,1};memcpy(guest+256,init,sizeof init);
 if(mode==9){const float green[4]={0,1,0,1};memcpy(guest+304,green,sizeof green);}
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 int st=vertex?0:1;graphics.nent[st]=1;nvn_gpu_layout_ent *e=&graphics.ent[st][0];
 e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;e->src_reg=0;e->src_reg2=UINT16_MAX;e->access=NVN_GPU_ACC_R;e->count=1;e->stages=1u<<(vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);
 nvn_gpu_layout layout={.ent=e,.nent=1};
 graphics.sh[0]=shader(argv[1],vertex?"compute_read_buffer.vert":"fullscreen.vert",NVN_GPU_SH_VS,vertex?&layout:NULL);
 graphics.sh[1]=shader(argv[1],vertex?"solid_green.frag":"compute_read_buffer.frag",NVN_GPU_SH_FS,vertex?NULL:&layout);graphics.color_out_mask=15;
 nvn_gpu_shader initial_vs=graphics.sh[0], initial_fs=graphics.sh[1], second_vs=NULL, second_fs=NULL;
 nvn_gpu_layout_ent second_entry={0};
 if(mode==11 || mode==12){
  REQUIRE(vertex);second_entry=*e;second_entry.stages=1u<<NVN_GPU_SH_FS;
  nvn_gpu_layout second_layout={.ent=&second_entry,.nent=1};
  second_vs=shader(argv[1],"fullscreen.vert",NVN_GPU_SH_VS,NULL);
  second_fs=shader(argv[1],"compute_multi_fragment.frag",NVN_GPU_SH_FS,&second_layout);
  rp_S.ssbo[1][0].addr=(uint64_t)(uintptr_t)(guest+304);rp_S.ssbo[1][0].size=16;
 }
 rp_S.ssbo[st][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[st][0].size=(mode==11 || mode==12)?48:64;
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL,mirror=NULL,other=NULL;uint64_t off=0;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,320,&mirror,&off));
 if(mode>=13 && mode<=16){
  GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));serial=3;GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 }
 pass(active,1);rp_pass_flush(active);
 const float green[4]={0,1,0,1};memcpy(guest+304,green,sizeof green);
 if(mode==9){ GPU(nvn_gpu_buf_create(64,NVN_GPU_MEM_SHARED,NULL,&other,&err)); nvn_gpu_buf_write(active,other,0,green,sizeof green);rp_draw_note_mirror_write(other,0,sizeof green); }
 else if(mode>=13 && mode<=16){
  untracked=1;REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,320,&mirror,&off));
  REQUIRE(inbound_pre && inbound_pre!=active);untracked=0;
 } else untracked=1;
 p_draw draw={0};draw.prim=4;draw.ind=(uint64_t)(uintptr_t)guest;rp_draw(active,&draw,NULL,&draw);
 if(mode==11 || mode==12){
  untracked=0;rp_pass_flush(active);graphics.nent[0]=0;graphics.nent[1]=1;graphics.ent[1][0]=second_entry;
  graphics.sh[0]=second_vs;graphics.sh[1]=second_fs;rp_dirty_all();
  rp_draw(active,&draw,NULL,&draw);rp_draw(active,&draw,NULL,&draw);
 }
 unsigned expected_draws=(mode==11 || mode==12)?3:1;
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);REQUIRE(draws==expected_draws&&skips==0&&!nvn_unimpl_kinds());
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);
 if(mode>=13 && mode<=16)GPU(nvn_gpu_cmd_commit(inbound_pre,2,done,NULL,&err));
 GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));
 if(mode>=13 && mode<=16){
  struct timespec deadline;REQUIRE(clock_gettime(CLOCK_REALTIME,&deadline)==0);deadline.tv_sec+=10;
  pthread_mutex_lock(&inbound_submit_mu);int rc=0;while(inbound_submits_done<3 && !rc)rc=pthread_cond_timedwait(&inbound_submit_cv,&inbound_submit_mu,&deadline);
  unsigned submitted=inbound_submits_done;pthread_mutex_unlock(&inbound_submit_mu);
  PFN_vkGetSemaphoreCounterValueKHR counter=(PFN_vkGetSemaphoreCounterValueKHR)__real_vkGetDeviceProcAddr(d.device,"vkGetSemaphoreCounterValueKHR");REQUIRE(counter);
  uint64_t value=~0ull;REQUIRE(counter(d.device,inbound_gate,&value)==VK_SUCCESS && value==0);
  PFN_vkSignalSemaphoreKHR signal=(PFN_vkSignalSemaphoreKHR)__real_vkGetDeviceProcAddr(d.device,"vkSignalSemaphoreKHR");REQUIRE(signal);
  VkSemaphoreSignalInfoKHR info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR,.semaphore=inbound_gate,.value=1};
  REQUIRE(signal(d.device,&info)==VK_SUCCESS);REQUIRE(!rc && submitted>=3);printf("INBOUND_PRE_GATE queued=%u counter_before=%llu\n",submitted,(unsigned long long)value);
 }
 GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=255||p[2]||p[3]!=255)bad++;}
 REQUIRE(ndraw==expected_draws&&inbound_transfers>=1);
 REQUIRE(inbound_injected==((mode && mode<5)?inbound_transfers:0));
 if(mode>=13 && mode<=16) REQUIRE(mirror_transfer_cb && mirror_barrier_cb && mirror_transfer_cb!=mirror_barrier_cb);
 if(mode==9 || mode==10) REQUIRE(mirror_barriers==0);
 if(mode==11 || mode==12){ REQUIRE(mirror_barriers==2); REQUIRE(mirror_stages[0]==(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)); REQUIRE(mirror_stages[1]==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT); REQUIRE(mirror_omitted==(unsigned)(mode==12?2:0)); }
 if((mode>=5 && mode<=7) || (mode>=13 && mode<=16)){
  VkPipelineStageFlags stages=VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | (vertex?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  VkAccessFlags accesses=VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  REQUIRE(mirror_barriers==1 && mirror_dst_stages==stages && mirror_dst_access==accesses);
  REQUIRE(mirror_omitted==(unsigned)(mode==6 || mode==7 || mode==14 || mode==15 || mode==16));
 }
 printf("INBOUND_TRANSFER dq=%d stage=%s inject=%d transfers=%u injected=%u publish=%u dst=%#x access=%#x two_levels=%d pixels_bad=%u\n",dq_mode,argv[2],mode,inbound_transfers,inbound_injected,mirror_barriers,mirror_dst_stages,mirror_dst_access,mirror_transfer_cb && mirror_barrier_cb && mirror_transfer_cb!=mirror_barrier_cb,bad);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(initial_vs);nvn_gpu_shader_release(initial_fs);if(second_vs)nvn_gpu_shader_release(second_vs);if(second_fs)nvn_gpu_shader_release(second_fs);nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);
 GPU(nvn_gpu_close(5000000000ull,&err));if(inbound_gate)vkDestroySemaphore(d.device,inbound_gate,NULL);REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
