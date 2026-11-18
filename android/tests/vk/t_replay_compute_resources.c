// SPDX-License-Identifier: Apache-2.0
// 同一真实回放夹具：间接命令由 CPU 写且不与计算输出相交，仅实际消费的资源触发依赖。
#define main indirect_fixture_main
#include "t_replay_compute_read.c"
#undef main
#include "rp_access.h"
static nvn_gpu_layout_ent entry(int cls,int kind,int access,int stage){
 nvn_gpu_layout_ent e={0};e.cls=(uint8_t)cls;e.src_kind=(uint8_t)kind;e.src_reg2=UINT16_MAX;
 e.count=1;e.access=(uint8_t)access;e.stages=1u<<stage;return e;
}
static void storage_spans(void){
 nvn_gpu_buf b=NULL;GPU(nvn_gpu_buf_create(1024,NVN_GPU_MEM_SHARED,NULL,&b,&err));
 nvn_gpu_view x=NULL,y=NULL;GPU(nvn_gpu_view_create_buf(b,256,512,NVN_FORMAT_RGBA32F,&x,&err));GPU(nvn_gpu_view_create_buf(b,512,256,NVN_FORMAT_RGBA32F,&y,&err));
 nvn_gpu_access_span bx,by,br;REQUIRE(nvn_gpu_view_access_span(x,&bx));REQUIRE(nvn_gpu_view_access_span(y,&by));REQUIRE(nvn_gpu_buf_access_span(b,256,512,&br));
 REQUIRE(bx.storage==br.storage&&by.storage==br.storage&&bx.off==256&&bx.bytes==512&&by.off==512&&by.bytes==256);
 REQUIRE(rp_access_intersects(&bx,&by));REQUIRE(!nvn_gpu_buf_access_span(b,1023,2,&br));
 uint64_t previous=bx.storage;nvn_gpu_view_release(x);nvn_gpu_view_release(y);nvn_gpu_buf_release(b,NULL,NULL);
 GPU(nvn_gpu_buf_create(1024,NVN_GPU_MEM_SHARED,NULL,&b,&err));REQUIRE(nvn_gpu_buf_access_span(b,256,512,&br));REQUIRE(br.storage!=previous);nvn_gpu_buf_release(b,NULL,NULL);
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_3D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=td.d.depth=4;td.d.levels=2;td.usage=NVN_GPU_TEX_STORAGE|NVN_GPU_TEX_SAMPLED;
 nvn_gpu_tex t=NULL;GPU(nvn_gpu_tex_create(&td,&t,&err));GPU(nvn_gpu_view_create(t,NULL,&x,&err));
 view_t desc={0};desc.set_target=desc.set_levels=desc.set_layers=1;desc.target=NVN_TEXTURE_TARGET_2D;desc.nlevels=desc.nlayers=1;desc.min_layer=2;
 GPU(nvn_gpu_view_create(t,&desc,&y,&err));REQUIRE(nvn_gpu_view_access_span(x,&bx));REQUIRE(nvn_gpu_view_access_span(y,&by));
 printf("STORAGE_SPAN 3d_levels=%u 3d_depth_slices=%u slice_layer=%u overlap=%d\n",bx.levels,bx.layers,by.layer,rp_access_intersects(&bx,&by));
 REQUIRE(bx.storage==by.storage&&bx.level==0&&bx.levels==2&&bx.layer==0&&bx.layers==4);REQUIRE(by.layer==2&&by.layers==1&&rp_access_intersects(&bx,&by));
 nvn_gpu_view_release(x);nvn_gpu_view_release(y);
 memset(&desc,0,sizeof desc);desc.set_levels=1;desc.base_level=1;desc.nlevels=1;GPU(nvn_gpu_view_create(t,&desc,&x,&err));
 REQUIRE(nvn_gpu_view_access_span(x,&bx));REQUIRE(bx.level==1&&bx.levels==1&&bx.layers==2);
 nvn_gpu_view_release(x);nvn_gpu_tex_release(t);puts("STORAGE_SPAN lifecycle/buffer-alias/3d-depth: PASS");
}
int main(int argc,char **argv){
 encoding_thread=1;
 REQUIRE(argc==3);const char *arm=argv[2];normalize_dependencies=1;
 disable_compute_access=strstr(arm,"disabled")!=NULL;
 int vertex=strstr(arm,"vertex")!=NULL,index=strstr(arm,"index")!=NULL;
 int vsbuf=strstr(arm,"vsbuf")!=NULL,fsbuf=strstr(arm,"fsbuf")!=NULL,tbuf=strstr(arm,"tbuf")!=NULL;
 int img=strstr(arm,"image")!=NULL,disjoint=strstr(arm,"disjoint")!=NULL,metadata=!strcmp(arm,"metadata");
 REQUIRE(vertex+index+vsbuf+fsbuf+tbuf+img+metadata==1);REQUIRE(!disjoint||img);
 struct s2a_vk_device d;char why[256];REQUIRE(!s2a_vk_device_create(&d,why,sizeof why));
 nvn_gpu_open_desc od={0};od.host_device=&d;od.app_label="replay-compute-resources";GPU(nvn_gpu_open(&od,&err));
 if(metadata){storage_spans();GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&d,1));return 0;}
 memset(guest,0,sizeof guest);const uint32_t args[5]={3,1,0,0,0};memcpy(guest,args,sizeof args);
 rp_state_reset();rp_draw_reset();nvn_unimpl_reset();
 rp_S.color.logic_op=NVN_LOGIC_OP_COPY;rp_S.chmask.mask=15;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;
 prog.is_compute=1;prog.nent[0]=1;
 prog.ent[0][0]=entry(img?NVN_GPU_RES_IMAGE:NVN_GPU_RES_SSBO,img?NVN_RB_IMG:NVN_RB_SBUFP,NVN_GPU_ACC_W,NVN_GPU_SH_CS);
 nvn_gpu_layout cl={0};cl.ent=prog.ent[0];cl.nent=1;
 prog.sh[0]=shader(argv[1],img?"compute_read_image.comp":index?"compute_read_index.comp":"compute_read_payload.comp",NVN_GPU_SH_CS,&cl);
 const char *vs=vertex?"compute_read_vertex.vert":vsbuf?"compute_read_buffer.vert":"fullscreen.vert";
 const char *fs=fsbuf?"compute_read_buffer.frag":tbuf?"compute_read_texel.frag":img?"compute_read_image.frag":"solid_green.frag";
 nvn_gpu_layout gl[2]={{0},{0}};
 if(vsbuf||fsbuf||tbuf||img){
  int st=vsbuf?0:1;graphics.nent[st]=1;
  graphics.ent[st][0]=entry(tbuf?NVN_GPU_RES_TBUF:img?NVN_GPU_RES_TEX_SAMP:NVN_GPU_RES_SSBO,tbuf?NVN_RB_TBUF:img?NVN_RB_TEX:NVN_RB_SBUFP,NVN_GPU_ACC_R,st?NVN_GPU_SH_FS:NVN_GPU_SH_VS);
  gl[st].ent=graphics.ent[st];gl[st].nent=1;
  rp_S.ssbo[st][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[st][0].size=64;
  if(tbuf)graphics.dtt[st][0]=NVN_TEXTURE_TARGET_BUFFER+1;
  if(img)graphics.dtt[st][0]=NVN_TEXTURE_TARGET_2D+1;
 }
 graphics.sh[0]=shader(argv[1],vs,NVN_GPU_SH_VS,gl[0].nent?&gl[0]:NULL);
 graphics.sh[1]=shader(argv[1],fs,NVN_GPU_SH_FS,gl[1].nent?&gl[1]:NULL);graphics.color_out_mask=15;
 rp_S.ssbo[5][0].addr=(uint64_t)(uintptr_t)(guest+256);rp_S.ssbo[5][0].size=index?16:64;
 if(vertex){graphics.attr_mask=1;rp_S.nattr=rp_S.nstream=1;rp_S.attr[0].fmt=NVN_FORMAT_RGBA32F;rp_S.stream[0].stride=16;rp_S.vbuf[0].address=(uint64_t)(uintptr_t)(guest+256);rp_S.vbuf[0].size=48;}
 if(tbuf){resources[1].valid=1;resources[1].tex.d.target=NVN_TEXTURE_TARGET_BUFFER;resources[1].tex.d.format=NVN_FORMAT_RGBA32F;resources[1].tex.d.width=4;resources[1].tex.d.levels=1;resources[1].tex.mem=guest+256;resources[1].tex.pool=(pool_t*)(void*)guest;rp_S.tex[1][0]=1;}
 nvn_gpu_tex image_tex=NULL;
 if(img){
  nvn_gpu_tex_desc it={0};it.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;it.d.format=NVN_FORMAT_RGBA8;it.d.width=it.d.height=it.d.levels=1;it.d.depth=2;it.d.flags=NVN_TEXTURE_FLAGS_IMAGE;it.usage=NVN_GPU_TEX_STORAGE|NVN_GPU_TEX_SAMPLED;
  GPU(nvn_gpu_tex_create(&it,&image_tex,&err));
  for(int i=1;i<=3;i++){
   resources[i].valid=resources[i].has_view=1;resources[i].tex.d=it.d;resources[i].tex.serial=20;
   view_t *v=&resources[i].desc;v->set_layers=v->set_levels=v->set_target=1;v->nlevels=v->nlayers=1;v->target=NVN_TEXTURE_TARGET_2D;v->min_layer=(uint16_t)(i==3);
   GPU(nvn_gpu_view_create(image_tex,v,&resources[i].view,&err));
  }
  REQUIRE(resources[1].view!=resources[2].view); // 写 / 读句柄不同，但必须命中同存储范围。
  samp_par sp={0};sp.minf=NVN_MIN_FILTER_NEAREST;sp.magf=NVN_MAG_FILTER_NEAREST;sp.ws=sp.wt=sp.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE;
  GPU(nvn_gpu_samp_create(&sp,&resource_sampler,&err));
  rp_S.img[5][0]=1;rp_S.tex[1][0]=(1ull<<20)|2;
  prog.dtt[0][0]=NVN_TEXTURE_TARGET_2D+1;
 }
 nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=NVN_FORMAT_RGBA8;td.d.width=td.d.height=64;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER;
 nvn_gpu_tex tex=NULL;GPU(nvn_gpu_tex_create(&td,&tex,&err));GPU(nvn_gpu_view_create(tex,NULL,&target_view,&err));target_tex.d=td.d;target_tex.serial=1;target.n=1;target.color[0]=&target_tex;
 nvn_gpu_buf rb=NULL;GPU(nvn_gpu_buf_create(16384,NVN_GPU_MEM_SHARED,NULL,&rb,&err));GPU(nvn_gpu_cmd_begin(NULL,&active,&err));
 nvn_gpu_buf mirror=NULL;uint64_t off=0;REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,320,&mirror,&off));
 pass(active,1);rp_pass_flush(active);
 if(img){const uint8_t green[4]={0,255,0,255};NVNcopyRegion r={0};r.width=r.height=r.depth=1;nvn_gpu_tex_upload(active,image_tex,0,1,&r,green,4,4);}
 p_dispatch dp={0};dp.x=dp.y=dp.z=1;rp_dispatch(active,&dp,NULL);
 p_draw dr={0};dr.prim=4;dr.ind=(uint64_t)(uintptr_t)guest;
 // VVL 1.4.341 对 indirect 顶点/索引访问留有 TODO，direct 提供已知范围以取得真实 RAW 阳性。
 if(vertex){dr.ind=0;dr.count=3;dr.instances=1;}
 p_draw_idx di={0};di.prim=4;di.itype=2;di.ind=(uint64_t)(uintptr_t)guest;di.ibuf=(uint64_t)(uintptr_t)(guest+256);if(index){di.ind=0;di.count=3;di.instances=1;}
 if(disjoint){rp_S.tex[1][0]=(1ull<<20)|3;rp_draw(active,&dr,NULL,&dr);rp_S.tex[1][0]=(1ull<<20)|2;}
 rp_draw(active,index?NULL:&dr,index?&di:NULL,index?(const void*)&di:(const void*)&dr);
 uint64_t draws=0,skips=0;rp_draw_stats(&draws,&skips);REQUIRE(draws==1u+(unsigned)disjoint&&skips==0);REQUIRE(!nvn_unimpl_kinds());
 rp_pass_flush(active);nvn_gpu_copy_tex_buf(active,tex,0,0,NULL,rb,0,256,16384);
 GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));
 REQUIRE(ndispatch==1);REQUIRE(ndraw==1u+(unsigned)disjoint);
 unsigned before=dispatch_compute_bar[0],after=draw_compute_bar[ndraw-1];
 REQUIRE(after-before==(unsigned)!disable_compute_access);
 if(disjoint){REQUIRE(draw_compute_bar[0]==before);printf("RESOURCE_READ layer=1 overlaps_writer=0 barrier_delta=%u\n",draw_compute_bar[0]-before);}
 if(img)printf("RESOURCE_READ layer=0 overlaps_writer=1 barrier_delta=%u\n",after-before);
 unsigned bad=0;const uint8_t *pixels=nvn_gpu_buf_cpu(rb);for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]||p[1]!=255||p[2]||p[3]!=255)bad++;}
 REQUIRE(!memcmp((const uint8_t*)nvn_gpu_buf_cpu(mirror)+off,args,sizeof args));
 printf("COMPUTE_RESOURCE arm=%s dq=%d draws=%llu pixel_mismatches=%u compute_barriers=%u\n",arm,dq_mode,(unsigned long long)draws,bad,after-before);
 for(int k=0;k<nw;k++)rp_buf_gpu_write_land(wkey[k],wid[k],0);
 rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(graphics.sh[0]);nvn_gpu_shader_release(graphics.sh[1]);
 if(img){for(int i=1;i<=3;i++)nvn_gpu_view_release(resources[i].view);nvn_gpu_samp_release(resource_sampler);nvn_gpu_tex_release(image_tex);}
 nvn_gpu_view_release(target_view);nvn_gpu_tex_release(tex);nvn_gpu_buf_release(rb,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));REQUIRE(!s2a_vk_device_destroy(&d,1));return bad?1:0;
}
