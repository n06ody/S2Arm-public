// SPDX-License-Identifier: Apache-2.0
// Real replay draws: graphics storage writer -> graphics storage reader.
#define main compute_read_original_main
#include "t_replay_compute_read.c"
#undef main
struct inbound_diag {uint64_t narrow, fallback, boundary, outside, draw, dispatch, multi, unknown, diag_other, diag_full, diag_game, diag_ct, diag_att, site_sum, site_overflow, pending_clean, pending_writes, pending_unknown, pending_war, pending_pre;};
static void inbound_diag_read(void *user,const char *name,uint64_t value){
 struct inbound_diag *d=user;
 if(!strcmp(name,"inbound_diag_draw_pre_unresolved"))d->pending_pre=value;else if(!strcmp(name,"inbound_diag_draw_no_nonatt_write"))d->pending_clean=value;else if(!strcmp(name,"inbound_diag_draw_nonatt_write"))d->pending_writes=value;else if(!strcmp(name,"inbound_diag_draw_source_unknown"))d->pending_unknown=value;else if(!strcmp(name,"inbound_diag_draw_no_nonatt_write_prior_read"))d->pending_war=value;
 else if(!strcmp(name,"inbound_narrow_passes"))d->narrow=value;else if(!strcmp(name,"inbound_fallback_passes"))d->fallback=value;
 else if(!strcmp(name,"inbound_fallback_boundary"))d->boundary=value;else if(!strcmp(name,"inbound_fallback_outside"))d->outside=value;
 else if(!strcmp(name,"inbound_fallback_draw"))d->draw=value;else if(!strcmp(name,"inbound_fallback_dispatch"))d->dispatch=value;
 else if(!strcmp(name,"inbound_fallback_multi_write"))d->multi=value;else if(!strcmp(name,"inbound_fallback_unknown"))d->unknown=value;
 else if(!strcmp(name,"inbound_diag_pass_other"))d->diag_other=value;else if(!strcmp(name,"inbound_diag_full_total"))d->diag_full=value;
 else if(!strcmp(name,"inbound_diag_pass_attachment"))d->diag_att=value;else if(!strcmp(name,"inbound_diag_pass_game"))d->diag_game=value;else if(!strcmp(name,"inbound_diag_pass_compute_transfer"))d->diag_ct=value;
 else if(!strcmp(name,"inbound_diag_full_overflow"))d->site_overflow=value;else if(!strncmp(name,"inbound_full_",13))d->site_sum+=value;
}
int main(int argc,char **argv){
 encoding_thread=1;const char *mode=getenv("S2A_GFX_ACCESS_MODE");int m=mode?atoi(mode):0;REQUIRE(argc==4);int src_vs=!strcmp(argv[2],"VS"),dst_vs=!strcmp(argv[3],"VS");
 REQUIRE(src_vs||!strcmp(argv[2],"FS"));REQUIRE(dst_vs||!strcmp(argv[3],"FS"));
 const char *hazard=getenv("S2A_GFX_HAZARD");int waw=hazard&&!strcmp(hazard,"WAW"),war=(hazard&&!strcmp(hazard,"WAR")) || ((m>=22 && m<=24) || m==27 || m==29 || m==31 || m==32 || m==33 || m==34);int xseg=getenv("S2A_GFX_XSEG")!=NULL;normalize_dependencies=1;track_all_draws=1;
 struct s2a_vk_device d;char why[256];REQUIRE(!s2a_vk_device_create(&d,why,sizeof why));
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-graphics-storage";GPU(nvn_gpu_open(&od,&err));
 if(xseg){REQUIRE(d.timeline_semaphore_enabled);VkSemaphoreTypeCreateInfoKHR type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE_KHR,.initialValue=0};
  VkSemaphoreCreateInfo info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};REQUIRE(vkCreateSemaphore(d.device,&info,NULL,&inbound_gate)==VK_SUCCESS);}

 memset(guest,0,sizeof guest);if(war){const float green[4]={0,1,0,1};memcpy(guest+304,green,sizeof green);}rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 nvn_gpu_layout_ent entry[2]={{0}};nvn_gpu_layout layouts[2]={{0}};
 for(int i=0;i<2;i++){
  int vertex=i?dst_vs:src_vs;entry[i].cls=NVN_GPU_RES_SSBO;entry[i].src_kind=NVN_RB_SBUFP;entry[i].src_reg2=UINT16_MAX;
  entry[i].access=(i?!waw&&!war:war)?NVN_GPU_ACC_R:NVN_GPU_ACC_W;entry[i].count=1;entry[i].stages=1u<<(vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);
  layouts[i].ent=&entry[i];layouts[i].nent=1;
  int stage=vertex?0:1;rp_S.ssbo[stage][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[stage][0].size=64;
 }
 nvn_gpu_shader shaders[2][2]={{0}};
 shaders[0][0]=shader(argv[1],src_vs?(war?"inbound_storage_read.vert":"inbound_storage_write.vert"):"fullscreen.vert",NVN_GPU_SH_VS,src_vs?&layouts[0]:NULL);
 nvn_gpu_layout_ent second_writer=entry[0];second_writer.binding=1;second_writer.stages=1u<<NVN_GPU_SH_FS;
 nvn_gpu_layout second_layout={.ent=&second_writer,.nent=1};
 shaders[0][1]=shader(argv[1],m==16?"inbound_storage_write_bind1.frag":src_vs?(war?"inbound_varying.frag":"inbound_black.frag"):(war?"compute_read_buffer.frag":"cgr_graphics_write.frag"),NVN_GPU_SH_FS,m==16?&second_layout:src_vs?NULL:&layouts[0]);
 shaders[1][0]=shader(argv[1],dst_vs?((waw||war)?"inbound_storage_write2.vert":"inbound_storage_read.vert"):"fullscreen.vert",NVN_GPU_SH_VS,dst_vs?&layouts[1]:NULL);
 shaders[1][1]=shader(argv[1],dst_vs?((waw||war)?"inbound_black.frag":"inbound_varying.frag"):((waw||war)?"inbound_storage_write2.frag":"compute_read_buffer.frag"),NVN_GPU_SH_FS,dst_vs?NULL:&layouts[1]);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL,tex2=NULL;nvn_gpu_view view2=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));
 if(war){GPU(nvn_gpu_tex_create(&td,&tex2,&err));GPU(nvn_gpu_view_create(tex2,NULL,&view2,&err));}target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL,mirror=NULL;uint64_t off=0;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,640,&mirror,&off));
 graphics.nent[src_vs?0:1]=1;graphics.ent[src_vs?0:1][0]=entry[0];
 if(m==16){REQUIRE(src_vs && !xseg);graphics.nent[1]=1;graphics.ent[1][0]=second_writer;rp_S.ssbo[1][0].addr=(uint64_t)(uintptr_t)(guest+512);rp_S.ssbo[1][0].size=64;}
 graphics.sh[0]=shaders[0][0];graphics.sh[1]=shaders[0][1];graphics.color_out_mask=15;
 p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;rp_draw(active,&draw,NULL,&draw);rp_pass_flush(active);
 if(xseg){GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));serial=2;GPU(nvn_gpu_cmd_begin(NULL,&active,&err));}
 nvn_gpu_view view1=target_view;if(war)target_view=view2;
 if(m==18)rp_pass_barrier(NVN_BARRIER_ORDER_PRIMITIVES_BIT);
 if(m==19){uint32_t v=0;nvn_gpu_buf_write(active,mirror,0,&v,sizeof v);}
 if(m==21 || m==24)nvn_gpu_inbound_diag_host_write();
 if(m==25)nvn_gpu_barrier(active,NVN_BARRIER_INVALIDATE_SHADER_BIT);
 if(m==26){nvn_gpu_barrier(active,NVN_BARRIER_ORDER_PRIMITIVES_BIT);nvn_gpu_inbound_diag_game_barrier(active);nvn_gpu_barrier(active,NVN_BARRIER_INVALIDATE_SHADER_BIT);}
 if(war && (m==9 || m==10))rp_S.ssbo[dst_vs?0:1][0].addr=(uint64_t)(uintptr_t)(guest+512);
 if(m==16)rp_S.ssbo[dst_vs?0:1][0].addr=(uint64_t)(uintptr_t)(guest+256);
 graphics.nent[0]=graphics.nent[1]=0;graphics.nent[dst_vs?0:1]=1;graphics.ent[dst_vs?0:1][0]=entry[1];graphics.sh[0]=shaders[1][0];graphics.sh[1]=shaders[1][1];rp_dirty_all();
 rp_draw(active,&draw,NULL,&draw);rp_pass_flush(active);
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);REQUIRE(draws==2&&skips==0&&!nvn_unimpl_kinds());
 nvn_gpu_copy_tex_buf(active,war?tex2:tex,0,0,NULL,rb,0,256,16384);if(m==22 || m==29)nvn_gpu_inbound_diag_precedes_main(active);GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));
 if(xseg){
  struct timespec deadline;REQUIRE(clock_gettime(CLOCK_REALTIME,&deadline)==0);deadline.tv_sec+=10;
  pthread_mutex_lock(&inbound_submit_mu);int rc=0;while(inbound_submits_done<2 && !rc)rc=pthread_cond_timedwait(&inbound_submit_cv,&inbound_submit_mu,&deadline);
  unsigned submitted=inbound_submits_done;pthread_mutex_unlock(&inbound_submit_mu);
  PFN_vkGetSemaphoreCounterValueKHR counter=(PFN_vkGetSemaphoreCounterValueKHR)__real_vkGetDeviceProcAddr(d.device,"vkGetSemaphoreCounterValueKHR");REQUIRE(counter);
  uint64_t value=~0ull;REQUIRE(counter(d.device,inbound_gate,&value)==VK_SUCCESS && value==0);
  PFN_vkSignalSemaphoreKHR signal=(PFN_vkSignalSemaphoreKHR)__real_vkGetDeviceProcAddr(d.device,"vkSignalSemaphoreKHR");REQUIRE(signal);
  VkSemaphoreSignalInfoKHR info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR,.semaphore=inbound_gate,.value=1};REQUIRE(signal(d.device,&info)==VK_SUCCESS);
  REQUIRE(!rc && submitted>=2);printf("GFX_GATE queued=%u counter_before=%llu\n",submitted,(unsigned long long)value);
 }
 GPU(nvn_gpu_fence_wait(serial,5000000000ull,&err));REQUIRE(ndraw==2&&gfx_injected==(unsigned)(m>=1 && m<=3));
 if(war && (m==9 || m==11))REQUIRE(gfx_exec==0);
 if(war && m==10){REQUIRE(gfx_exec==1 && !gfx_exec_omitted);REQUIRE(gfx_exec_src==(src_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));REQUIRE(gfx_exec_dst==(dst_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));}
 if(war && (m>=4 && m<=6)){
  REQUIRE(gfx_exec==1 && gfx_exec_omitted==(unsigned)(m==5 || m==6));
  REQUIRE(gfx_exec_src==(src_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
  REQUIRE(gfx_exec_dst==(dst_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
 }
 if(waw && (m==4 || m==5)){REQUIRE(gfx_full==1 && gfx_full_omitted==(unsigned)(m==5) && gfx_published==0);}
 if(!waw && !war && ((m>=4 && m<=6) || m==14)){REQUIRE(gfx_published==1);REQUIRE(gfx_src_stages==(src_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));REQUIRE(gfx_dst_stages==(dst_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));REQUIRE(gfx_src_access==VK_ACCESS_SHADER_WRITE_BIT && gfx_dst_access==VK_ACCESS_SHADER_READ_BIT);REQUIRE(gfx_omitted==(unsigned)(m==5 || m==6));}
 if(m==7 || m==8 || m==11 || m==12 || m==13 || m==15) REQUIRE(gfx_published==0);
 if(m==12 || m==13) REQUIRE(inbound_fallback==1 && inbound_fallback_omitted==(unsigned)(m==13) && inbound_narrow_passes>=2);
 if(m==14) REQUIRE(inbound_fallback==0 && inbound_narrow_passes>=2);
 if(m==16) REQUIRE(inbound_fallback==1 && inbound_narrow_passes>=2);
 if(m==15) REQUIRE(inbound_fallback==0 && inbound_old_passes>=2);
 if(m>=12 && m<=34){
  struct inbound_diag diag={0};nvn_gpu_stats(inbound_diag_read,&diag,0);
  printf("INBOUND_DIAG narrow=%llu fallback=%llu boundary=%llu outside=%llu draw=%llu dispatch=%llu multi=%llu unknown=%llu\n",
   (unsigned long long)diag.narrow,(unsigned long long)diag.fallback,(unsigned long long)diag.boundary,
   (unsigned long long)diag.outside,(unsigned long long)diag.draw,(unsigned long long)diag.dispatch,(unsigned long long)diag.multi,(unsigned long long)diag.unknown);
  if(m==12 || m==13){REQUIRE(diag.fallback>=2 && diag.boundary>=1);if(!xseg)REQUIRE(diag.draw>=1);}
  if(m==14)REQUIRE(diag.narrow>=1 && diag.fallback>=1 && diag.boundary>=1);
  if(m==16)REQUIRE(diag.fallback>=2 && diag.draw>=1 && diag.multi>=1);
  if(m>=27)printf("PENDING_DIAG no_nonatt_write=%llu prior_read=%llu nonatt_write=%llu unknown=%llu pre_unresolved=%llu\n",(unsigned long long)diag.pending_clean,(unsigned long long)diag.pending_war,(unsigned long long)diag.pending_writes,(unsigned long long)diag.pending_unknown,(unsigned long long)diag.pending_pre);
  if(m>=17)printf("INBOUND_SOURCE game=%llu attachment=%llu compute_transfer=%llu other=%llu full=%llu site_sum=%llu overflow=%llu\n",(unsigned long long)diag.diag_game,(unsigned long long)diag.diag_att,(unsigned long long)diag.diag_ct,(unsigned long long)diag.diag_other,(unsigned long long)diag.diag_full,(unsigned long long)diag.site_sum,(unsigned long long)diag.site_overflow);
  if(m>=17 && m!=30){REQUIRE(diag.diag_other>=1 && diag.diag_full>=1);REQUIRE(diag.site_sum==diag.diag_full && !diag.site_overflow);}
  if(m==18)REQUIRE(diag.diag_game>=1);
  if(m==19)REQUIRE(diag.diag_ct>=1);
  if(m==23)REQUIRE(diag.diag_att>=1);
  if(m==22 || m==24)REQUIRE(diag.diag_att==0 && diag.diag_other>=2);
  if(m==25 || m==26)REQUIRE(diag.diag_game==0);
  if(m==27 || m==31 || m==32 || m==33 || m==34)REQUIRE(diag.pending_clean==1 && diag.pending_war==1 && diag.pending_writes==0 && diag.pending_unknown==0 && diag.pending_pre==0);
  if(m==31 || m==33)REQUIRE(inbound_fallback==1 && inbound_fallback_omitted==1);
  if(m==32 || m==34)REQUIRE(inbound_fallback==1 && inbound_fallback_omitted==0);
  if(m==30)REQUIRE(diag.pending_clean==0 && diag.pending_writes==0 && diag.pending_unknown==0);
  if(m==28)REQUIRE(diag.pending_writes==1 && diag.pending_clean==0 && diag.pending_unknown==0);
  if(m==29)REQUIRE(diag.pending_unknown==1 && diag.pending_clean==0 && diag.pending_writes==0 && diag.pending_pre==1);
  if(m==15)REQUIRE(!diag.narrow && !diag.fallback);
 }
 if(waw||war){const float *words=(const float *)(const void *)(nvn_gpu_buf_cpu(mirror)+(war && (m==9 || m==10)?560:304));REQUIRE(words[0]==1 && words[1]==0 && words[2]==0 && words[3]==1);}
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=((waw||war)?0:255)||p[2]||p[3]!=255)bad++;}
 printf("GFX_STORAGE dq=%d src=%s dst=%s mode=%d xseg=%d draws=%u injected=%u publish=%u full=%u omitted_full=%u exec=%u omitted_exec=%u fallback=%u omitted_fallback=%u narrow_passes=%u old_passes=%u src=%#x dst=%#x pixels_bad=%u\n",dq_mode,argv[2],argv[3],m,xseg,ndraw,gfx_injected,gfx_published,gfx_full,gfx_full_omitted,gfx_exec,gfx_exec_omitted,inbound_fallback,inbound_fallback_omitted,inbound_narrow_passes,inbound_old_passes,gfx_src_stages,gfx_dst_stages,bad);
 for(int i=0;i<nw;i++)rp_buf_gpu_write_land(wkey[i],wid[i],0);
 rp_draw_reset();rp_buf_reset();for(int i=0;i<2;i++)for(int st=0;st<2;st++)nvn_gpu_shader_release(shaders[i][st]);
 nvn_gpu_view_release(view1);if(view2)nvn_gpu_view_release(view2);nvn_gpu_tex_release(tex);if(tex2)nvn_gpu_tex_release(tex2);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));if(inbound_gate)vkDestroySemaphore(d.device,inbound_gate,NULL);REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
