// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production record/replay with a controlled frame-queue and Vulkan boundary; no GPU execution.
#include "../native/direct/vk_record.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct s2a_vk_device dev;
static uint64_t frame = 1;
static int gate, eligible = 1, reject, accepted_failure, submits, breaks[2], finishes, unknown, enqueue_count;
static int end_count, command_freed[256], owner_live[256], owner_cmd[256];
static int guest_value, built_value;
static int pool_active=1, pool_detached, pool_completed;
static unsigned next_cmd, next_owner;
static uint64_t command_frame[256];
static unsigned queued[256], queued_n;
static int rd(void *o,uint64_t a,void *b,size_t n){(void)o;memcpy(b,(void*)(uintptr_t)a,n);return 0;}
static int wr(void *o,uint64_t a,const void *b,size_t n){(void)o;memcpy((void*)(uintptr_t)a,b,n);return 0;}
static struct s2a_record_memory mem={NULL,rd,wr};
static int packet(unsigned op,const void*p,size_t n){
 uint8_t b[4096]={0};size_t z=(sizeof(op_hdr)+n+7)&~(size_t)7;
 op_hdr h={op,z};memcpy(b,&h,sizeof h);if(n)memcpy(b+sizeof h,p,n);
 struct s2a_record_result r;return s2a_record_exec_one(&dev,b,z,&mem,&r);
}
static p_bind_program snap;
const prog_t *nvn_prog_snap_get(uint64_t serial,uint8_t keys[6][8],uint8_t *ok,uint8_t stage[6]){
 if(serial!=1)return NULL;memcpy(keys,snap.keys,sizeof snap.keys);*ok=snap.kok;
 memcpy(stage,snap.stage_of,sizeof snap.stage_of);return &snap.snap;
}
static void bind(void){
 memset(&snap,0,sizeof snap);snap.snap.serial=1;snap.kok=1;memset(snap.stage_of,255,6);snap.stage_of[0]=5;
 p_bind_program_id p={.prog=(void*)1,.serial=1,.stages=32};assert(packet(OP_BIND_PROGRAM,&p,sizeof p)==0);
}
static void free_through(uint64_t f){
 for(unsigned i=0;i<queued_n;i++)if(queued[i] && command_frame[queued[i]]<=f){
  assert(!command_freed[queued[i]]);command_freed[queued[i]]=1;queued[i]=0;
 }
}
extern void s2a_record_fq_retire(uint64_t completed_frame) __attribute__((weak));
int nvn_vk_first_draw_pass_break(int sync){assert(sync==0||sync==1);breaks[sync]++;
 int completed = 0;
 // Only live work from this frame is flushed here; older deferred slots are not retired.
 if(sync) for(unsigned i=0;i<queued_n;i++)if(queued[i] && command_frame[queued[i]]==frame){
  assert(!command_freed[queued[i]]);command_freed[queued[i]]=1;queued[i]=0;completed=1;
 }
 if(completed){guest_value=1;if(!pool_detached)pool_active=0;if(s2a_record_fq_retire)s2a_record_fq_retire(frame);frame++;}
 return 0;
}
#ifndef TEST_NO_FRAMEID
uint64_t nvn_vk_first_draw_frame_id(void){return frame;}
#endif
#ifndef TEST_NO_DETACH
uint64_t nvn_vk_first_draw_detach_submit_seq(void){pool_detached=1;return 7;}
#endif
#ifndef TEST_NO_POOL_COMPLETE
void s2a_poolahb_complete_seq(uint64_t seq){assert(seq==7&&pool_active&&pool_detached);pool_active=0;pool_completed++;}
#endif
#ifndef TEST_NO_ENQUEUE
int nvn_vk_first_draw_enqueue(struct s2a_vk_device*d,VkCommandBuffer c){(void)d;
 if(reject)return -1;unsigned id=(unsigned)(uintptr_t)c;command_frame[id]=frame;queued[queued_n++]=id;enqueue_count++;return accepted_failure ? 1 : 0;
}
#endif
// New completion-proof seam is weak here so the same executable can test the inherited WIP red.
static void retire(uint64_t f){free_through(f);if(s2a_record_fq_retire)s2a_record_fq_retire(f);else s2a_record_fq_drain(1);}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo*i,VkCommandBuffer*c){(void)d;(void)i;assert(next_cmd<255);*c=(VkCommandBuffer)(uintptr_t)++next_cmd;owner_cmd[next_owner]=next_cmd;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo*i){(void)c;(void)i;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){(void)c;end_count++;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer*c){(void)d;(void)p;assert(n==1);unsigned id=(unsigned)(uintptr_t)*c;assert(!command_freed[id]);command_freed[id]=1;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo*i,const VkAllocationCallbacks*a,VkFence*f){(void)d;(void)i;(void)a;*f=(VkFence)1;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks*a){(void)d;(void)f;(void)a;}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){(void)q;(void)n;(void)i;(void)f;if(!eligible)assert(built_value==guest_value);if(reject)assert(pool_active && "pooled backing retired before rejected command submit");submits++;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d,uint32_t n,const VkFence*f,VkBool32 a,uint64_t t){(void)d;(void)n;(void)f;(void)a;(void)t;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline v){(void)c;(void)p;(void)v;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t f,uint32_t n,const VkDescriptorSet*s,uint32_t dc,const uint32_t*ds){(void)c;(void)p;(void)l;(void)f;(void)n;(void)s;(void)dc;(void)ds;}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){(void)c;assert(x==2&&y==3&&z==4);}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier*m,uint32_t nb,const VkBufferMemoryBarrier*b,uint32_t ni,const VkImageMemoryBarrier*i){
 (void)c;(void)dep;(void)b;(void)i;assert(nm==1&&m&&nb==0&&ni==0);
 if(s==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT){assert(d==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);assert(m->srcAccessMask==VK_ACCESS_MEMORY_WRITE_BIT);assert(m->dstAccessMask==(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT));}
 else if(d & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT){assert(s==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);assert((d&VK_PIPELINE_STAGE_HOST_BIT)&&(m->dstAccessMask&VK_ACCESS_HOST_READ_BIT));}
 else {assert(s==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&d==VK_PIPELINE_STAGE_HOST_BIT&&(m->dstAccessMask&VK_ACCESS_HOST_READ_BIT));}
}
static int finished(struct s2a_vk_device*d,void*o,int completion){(void)d;unsigned id=(unsigned)(uintptr_t)o;assert(id&&owner_live[id]);
 if(completion==-2){unknown++;return 0;}assert(completion==0||completion==-1);
 // Queue owner may be retired only after its actual command is complete and released.
 assert(!owner_cmd[id] || command_freed[owner_cmd[id]]);owner_live[id]=0;if(completion==0)finishes++;return 0;
}
static int builder(struct s2a_vk_device*d,const struct s2a_record_dispatch_state*s,const struct s2a_record_memory*m,struct s2a_record_dispatch_output*out,struct s2a_record_result*r,void*o){
 (void)d;(void)m;(void)r;(void)o;assert(!s->fq_pool || gate);built_value=guest_value;out->pipeline=(VkPipeline)1;out->layout=(VkPipelineLayout)1;
 out->fq=eligible;unsigned id=++next_owner;assert(id<256);out->owner=(void*)(uintptr_t)id;owner_live[id]=1;out->finish=finished;return 0;
}
static int dispatch(void){p_dispatch d={.x=2,.y=3,.z=4};return packet(OP_DISPATCH,&d,sizeof d);}
static uint64_t pending(void){uint64_t n;s2a_record_dispatch_fq_stats(NULL,NULL,&n);return n;}
int main(int argc,char **argv){assert(argc==2);const char *mode=argv[1];gate=strcmp(mode,"off")!=0;
 setenv("S2A_DISPATCH_FQ",gate?"1":"0",1);dev.device=(VkDevice)1;s2a_record_reset_state();bind();s2a_record_set_dispatch_builder(builder,NULL);
 if(!strcmp(mode,"off")){assert(dispatch()==0);assert(breaks[1]==1&&breaks[0]==0&&submits==1&&enqueue_count==0&&finishes==1&&pending()==0);}
 else if(!strcmp(mode,"enqueue")){assert(dispatch()==0);assert(breaks[0]==1&&breaks[1]==0&&submits==0&&pending()==1);s2a_record_fq_drain(0);assert(unknown==1&&pending()==1&&owner_live[1]);frame=2;retire(1);assert(finishes==1&&pending()==0);}
 else if(!strcmp(mode,"retire")){assert(dispatch()==0);frame=2;assert(dispatch()==0);frame=3;retire(1);assert(finishes==1&&pending()==1&&owner_live[2]);retire(2);assert(finishes==2&&pending()==0);}
 else if(!strcmp(mode,"fallback")){assert(dispatch()==0);eligible=0;assert(dispatch()==0);assert(breaks[0]==2&&breaks[1]==1&&submits==1&&finishes==2&&pending()==0);}
 else if(!strcmp(mode,"deferred")){assert(dispatch()==0);frame=2;eligible=0;assert(dispatch()==0);assert(finishes==1&&pending()==1&&owner_live[1]);retire(1);assert(finishes==2&&pending()==0);}
 else if(!strcmp(mode,"reject-prior")||!strcmp(mode,"reject-unknown")){
  assert(dispatch()==0);reject=1;if(!strcmp(mode,"reject-unknown"))s2a_record_set_submit_fault(1);
  int rc=dispatch();
  if(!strcmp(mode,"reject-unknown")){assert(rc==S2A_RECORD_GPU&&pool_active&&pool_completed==0&&unknown==1);
   uint32_t left=99;assert(s2a_record_release_retained(&dev,1000000000,&left)==1&&left==0);
  }else assert(rc==0);
  assert(finishes==2&&pool_completed==1&&!pool_active&&pending()==0);
 }
 else if(!strcmp(mode,"accepted-failure")){accepted_failure=1;assert(dispatch()==S2A_RECORD_GPU);assert(submits==0&&enqueue_count==1&&pending()==1&&unknown==1&&owner_live[1]&&!command_freed[1]);}
 else if(!strcmp(mode,"reject")){reject=1;assert(dispatch()==0);assert(breaks[0]==1&&breaks[1]==1&&submits==1&&end_count==1&&finishes==1&&pending()==0);}
 else if(!strcmp(mode,"full")){for(int i=0;i<64;i++)assert(dispatch()==0);assert(pending()==64);assert(dispatch()==0);assert(breaks[1]==1&&submits==1&&finishes==65&&pending()==0);}
 else if(!strcmp(mode,"no-seam")){assert(dispatch()==0);assert(breaks[0]==0&&breaks[1]==1&&submits==1&&finishes==1&&pending()==0);}
 else abort();printf("dispatch-fq %s PASS\n",mode);return 0;
}
