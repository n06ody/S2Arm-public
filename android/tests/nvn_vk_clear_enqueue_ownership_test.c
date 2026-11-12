// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real first-draw queue + real clear recorder; Vulkan object operations are host mocks.
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../native/nvn/nvn_vk_first_draw.c"
#include "../native/direct/vk_clear.h"
static unsigned freed, submitted, allocated;
static unsigned test_end_passes;
static const char *mode;
int __system_property_get(const char *name,char *value){
 const char *s = !strcmp(name,"debug.s2a.counter_q") && !strcmp(mode,"counter-on") ? "1" : !strcmp(name,"debug.s2a.fail_submit") && !strcmp(mode,"injected") ? "1" : !strcmp(name,"debug.s2a.batch_passes") ? (!strcmp(mode,"queued") ? "2" : "1") : "";
 strcpy(value,s);return (int)strlen(s);
}
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo*i,const VkAllocationCallbacks*a,VkFence*o){(void)d;(void)i;(void)a;*o=(VkFence)7;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks*a){(void)d;(void)f;(void)a;}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){(void)q;(void)f;assert(n==1&&i->commandBufferCount==1&&i->pCommandBuffers[0]==(VkCommandBuffer)5);submitted++;return (!strcmp(mode,"submit-fail")||!strcmp(mode,"prior-fail"))?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo*i,const VkAllocationCallbacks*a,VkImageView*o){(void)d;(void)i;(void)a;*o=(VkImageView)2;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo*i,const VkAllocationCallbacks*a,VkRenderPass*o){(void)d;(void)i;(void)a;*o=(VkRenderPass)3;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo*i,const VkAllocationCallbacks*a,VkFramebuffer*o){(void)d;(void)i;(void)a;*o=(VkFramebuffer)4;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo*i,VkCommandBuffer*o){(void)d;(void)i;allocated++;*o=(VkCommandBuffer)5;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo*i){(void)c;(void)i;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){(void)c;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo*i,VkSubpassContents s){(void)c;(void)i;(void)s;}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c){(void)c;test_end_passes++;}
VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer c,uint32_t a,const VkClearAttachment*b,uint32_t n,const VkClearRect*r){(void)c;(void)a;(void)b;(void)n;(void)r;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer*c){(void)d;(void)p;assert(n==1);freed++;
 for(int i=0;i<g_fq.ncmd;i++)assert(g_fq.cmds[i]!=*c && "freed command remains owned by frame queue");
}
static int guest_read(void *o,uint64_t a,void *b,size_t n){(void)o;memcpy(b,(void*)(uintptr_t)a,n);return 0;}
static int guest_write(void *o,uint64_t a,const void *b,size_t n){(void)o;memcpy((void*)(uintptr_t)a,b,n);return 0;}
static size_t append(uint8_t *stream,size_t pos,uint32_t op,const void *p,size_t n){
 size_t size=(sizeof(op_hdr)+n+7)&~(size_t)7;op_hdr h={op,(uint32_t)size};memcpy(stream+pos,&h,sizeof h);
 if(p)memcpy(stream+pos+sizeof h,p,n);return pos+size;
}
static void record_failure(struct s2a_vk_device *d,int depth){
 uint8_t stream[4096]={0};size_t pos=0;tex_t tex={0};tex.magic=0x00584554;
 tex.d.format=depth?NVN_FORMAT_DEPTH32F:NVN_FORMAT_RGBA8;tex.d.width=tex.d.height=16;tex.d.levels=1;tex.serial=91;
 p_set_rt rt={0};if(depth)rt.depth=&tex;else{rt.n=1;rt.color[0]=&tex;}
 size_t len=(sizeof(op_hdr)+set_rt_size(&rt)+7)&~(size_t)7;op_hdr h={OP_SET_RT,(uint32_t)len};
 memcpy(stream+pos,&h,sizeof h);set_rt_put(stream+pos+sizeof h,&rt);pos+=len;
 if(depth){p_clear_ds v={.depth=1,.depth_mask=1};pos=append(stream,pos,OP_CLEAR_DS,&v,sizeof v);pos=append(stream,pos,OP_CLEAR_DS,&v,sizeof v);}
 else{p_clear_color v={.index=0,.mask=15};pos=append(stream,pos,OP_CLEAR_COLOR,&v,sizeof v);pos=append(stream,pos,OP_CLEAR_COLOR,&v,sizeof v);}
 pos=append(stream,pos,OP_END,NULL,0);
 struct s2a_replay_texture texture={.key=&tex,.image=(VkImage)1,.format=depth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM,.width=16,.height=16,.depth=1,.serial=91,.levels=1,.layers=1};
 struct s2a_record_memory memory={.read=guest_read,.write=guest_write};struct s2a_record_result result;
 int rc=s2a_record_replay(d,stream,stream+pos,&memory,&texture,1,NULL,0,NULL,0,&result);
 assert(rc==S2A_RECORD_GPU && result.opcode==(depth?OP_CLEAR_DS:OP_CLEAR_COLOR));
 assert(strstr(result.why,"frame queue failed after taking command"));
 assert(allocated==1 && g_fq.ncmd==1 && g_fq.state==FD_FQ_FAILED && freed==0);
}
int main(int argc,char **argv){
 assert(argc==3);mode=argv[1];int is_record=!strncmp(argv[2],"record-",7);int depth=strstr(argv[2],"depth")!=NULL;
 struct s2a_vk_device d={0};d.device=(VkDevice)1;d.queue=(VkQueue)1;d.command_pool=(VkCommandPool)1;if(strcmp(mode,"refused"))g_fq.dev=&d;
 if(!strcmp(mode,"full"))g_fq.ncmd=fd_fq_cmd_capacity();
 if(!strcmp(mode,"prior-fail")){g_fq.ncmd=g_fq.submitted_n=1;g_fq.cmds[0]=(VkCommandBuffer)6;g_fq.nbatch_fence=1;g_fq.batch_fence[0]=(VkFence)8;}
 if(is_record){record_failure(&d,depth);printf("%s record %s PASS\n",mode,depth?"depth":"color");return 0;}
 s2a_clear_target target={0};target.image=(VkImage)1;target.width=16;target.height=16;target.format=depth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;target.aspect=depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
 VkClearColorValue v={{0,0,0,1}};char why[256]={0};
 int rc=depth?s2a_vk_clear_ds_queued(&d,&target,1.0f,0,1,0,NULL,why,sizeof why):s2a_vk_clear_color_queued(&d,&target,&v,15,NULL,why,sizeof why);
 if(!strcmp(mode,"injected")||!strcmp(mode,"submit-fail")||!strcmp(mode,"prior-fail")) {
  assert(rc==S2A_VK_CLEAR_FQ_FAILED && g_fq.ncmd==(!strcmp(mode,"prior-fail")?2:1) && g_fq.state==FD_FQ_FAILED && freed==0);
  assert(strstr(why,"frame queue failed after taking command")&&strstr(why,!strcmp(mode,"injected")?"injected batch submit failure":"vkQueueSubmit failed"));
  assert(submitted==(!strcmp(mode,"injected")?0u:1u));
  if(!strcmp(mode,"prior-fail"))assert(g_fq.submitted_n==1&&g_fq.batch_fence[0]==(VkFence)8);
 } else if(!strcmp(mode,"refused")) assert(rc==S2A_VK_CLEAR_UNHANDLED&&freed==1&&g_fq.ncmd==0);
 else if(!strcmp(mode,"full")) assert(rc==S2A_VK_CLEAR_UNHANDLED&&freed==1&&g_fq.ncmd==fd_fq_cmd_capacity());
 else if(!strcmp(mode,"queued")) assert(rc==S2A_VK_CLEAR_QUEUED&&freed==0&&submitted==0&&g_fq.ncmd==1);
 else if(!strcmp(mode,"submitted")) assert(rc==S2A_VK_CLEAR_QUEUED&&freed==0&&submitted==1&&g_fq.submitted_n==1);
 else assert(0);
 printf("%s %s PASS\n",mode,depth?"depth":"color");return 0;
}
