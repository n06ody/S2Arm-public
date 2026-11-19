// SPDX-License-Identifier: Apache-2.0
// Real replay draws: graphics storage writer -> graphics storage reader.
#define main compute_read_original_main
#include "t_replay_compute_read.c"
#undef main
int main(int argc,char **argv){
 encoding_thread=1;REQUIRE(argc==4);int src_vs=!strcmp(argv[2],"VS"),dst_vs=!strcmp(argv[3],"VS");
 REQUIRE(src_vs||!strcmp(argv[2],"FS"));REQUIRE(dst_vs||!strcmp(argv[3],"FS"));
 int waw=getenv("S2A_GFX_HAZARD")!=NULL;int xseg=getenv("S2A_GFX_XSEG")!=NULL;normalize_dependencies=1;track_all_draws=1;
 struct s2a_vk_device d;char why[256];REQUIRE(!s2a_vk_device_create(&d,why,sizeof why));
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-graphics-storage";GPU(nvn_gpu_open(&od,&err));
 if(xseg){REQUIRE(d.timeline_semaphore_enabled);VkSemaphoreTypeCreateInfoKHR type={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE_KHR,.initialValue=0};
  VkSemaphoreCreateInfo info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&type};REQUIRE(vkCreateSemaphore(d.device,&info,NULL,&inbound_gate)==VK_SUCCESS);}

 memset(guest,0,sizeof guest);rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 nvn_gpu_layout_ent entry[2]={{0}};nvn_gpu_layout layouts[2]={{0}};
 for(int i=0;i<2;i++){
  int vertex=i?dst_vs:src_vs;entry[i].cls=NVN_GPU_RES_SSBO;entry[i].src_kind=NVN_RB_SBUFP;entry[i].src_reg2=UINT16_MAX;
  entry[i].access=i && !waw?NVN_GPU_ACC_R:NVN_GPU_ACC_W;entry[i].count=1;entry[i].stages=1u<<(vertex?NVN_GPU_SH_VS:NVN_GPU_SH_FS);
  layouts[i].ent=&entry[i];layouts[i].nent=1;
  int stage=vertex?0:1;rp_S.ssbo[stage][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[stage][0].size=64;
 }
 nvn_gpu_shader shaders[2][2]={{0}};
 shaders[0][0]=shader(argv[1],src_vs?"inbound_storage_write.vert":"fullscreen.vert",NVN_GPU_SH_VS,src_vs?&layouts[0]:NULL);
 shaders[0][1]=shader(argv[1],src_vs?"inbound_black.frag":"cgr_graphics_write.frag",NVN_GPU_SH_FS,src_vs?NULL:&layouts[0]);
 shaders[1][0]=shader(argv[1],dst_vs?(waw?"inbound_storage_write2.vert":"inbound_storage_read.vert"):"fullscreen.vert",NVN_GPU_SH_VS,dst_vs?&layouts[1]:NULL);
 shaders[1][1]=shader(argv[1],dst_vs?(waw?"inbound_black.frag":"inbound_varying.frag"):(waw?"inbound_storage_write2.frag":"compute_read_buffer.frag"),NVN_GPU_SH_FS,dst_vs?NULL:&layouts[1]);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL,mirror=NULL;uint64_t off=0;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,320,&mirror,&off));
 graphics.nent[src_vs?0:1]=1;graphics.ent[src_vs?0:1][0]=entry[0];graphics.sh[0]=shaders[0][0];graphics.sh[1]=shaders[0][1];graphics.color_out_mask=15;
 p_draw draw={0};draw.prim=4;draw.count=3;draw.instances=1;rp_draw(active,&draw,NULL,&draw);rp_pass_flush(active);
 if(xseg){GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));serial=2;GPU(nvn_gpu_cmd_begin(NULL,&active,&err));}
 graphics.nent[0]=graphics.nent[1]=0;graphics.nent[dst_vs?0:1]=1;graphics.ent[dst_vs?0:1][0]=entry[1];graphics.sh[0]=shaders[1][0];graphics.sh[1]=shaders[1][1];rp_dirty_all();
 rp_draw(active,&draw,NULL,&draw);rp_pass_flush(active);
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);REQUIRE(draws==2&&skips==0&&!nvn_unimpl_kinds());
 const char *mode=getenv("S2A_GFX_ACCESS_MODE");int m=mode?atoi(mode):0;
 nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);GPU(nvn_gpu_cmd_commit(active,serial,done,NULL,&err));
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
 if(waw && (m==4 || m==5)){REQUIRE(gfx_full==1 && gfx_full_omitted==(unsigned)(m==5) && gfx_published==0);}
 if(!waw && m>=4 && m<=6){REQUIRE(gfx_published==1);REQUIRE(gfx_src_stages==(src_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));REQUIRE(gfx_dst_stages==(dst_vs?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));REQUIRE(gfx_src_access==VK_ACCESS_SHADER_WRITE_BIT && gfx_dst_access==VK_ACCESS_SHADER_READ_BIT);REQUIRE(gfx_omitted==(unsigned)(m==5 || m==6));}
 if(m==7 || m==8) REQUIRE(gfx_published==0);
 if(waw){const float *words=(const float *)(const void *)(nvn_gpu_buf_cpu(mirror)+304);REQUIRE(words[0]==1 && words[1]==0 && words[2]==0 && words[3]==1);}
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=(waw?0:255)||p[2]||p[3]!=255)bad++;}
 printf("GFX_STORAGE dq=%d src=%s dst=%s mode=%d xseg=%d draws=%u injected=%u publish=%u full=%u omitted_full=%u src=%#x dst=%#x pixels_bad=%u\n",dq_mode,argv[2],argv[3],m,xseg,ndraw,gfx_injected,gfx_published,gfx_full,gfx_full_omitted,gfx_src_stages,gfx_dst_stages,bad);
 for(int i=0;i<nw;i++)rp_buf_gpu_write_land(wkey[i],wid[i],0);
 rp_draw_reset();rp_buf_reset();for(int i=0;i<2;i++)for(int st=0;st<2;st++)nvn_gpu_shader_release(shaders[i][st]);
 nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));if(inbound_gate)vkDestroySemaphore(d.device,inbound_gate,NULL);REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
