// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real first-draw queue + real clear recorder; Vulkan object operations are host mocks.
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "../native/nvn/nvn_vk_first_draw.c"
#include "../native/direct/vk_clear.h"
static unsigned freed, submitted, view_new, fb_new, view_freed, fb_freed, image_freed, cmds_new;
static int completed;
static char stat_line[512];
static const char *mode;
static struct nvn_vk_tex *record_owners;
uint64_t s2a_vk_adapter_registry_generation(void) { return 1; }
void s2a_vk_adapter_mark_written(VkImage image) {
 for (int i=0; record_owners && i<25; i++) if (record_owners[i].image==image) { record_owners[i].gpu_written=1; return; }
}
int __system_property_get(const char *name,char *value){
 const char *s = !strcmp(name,"debug.s2a.clear_fv_q") ? (!strcmp(mode,"off") ? "" : "1") : !strcmp(name,"debug.s2a.batch_passes") ? (!strcmp(mode,"failed-owned") ? "25" : "512") : !strcmp(name,"debug.s2a.fail_submit") && !strcmp(mode,"failed-owned") ? "1" : "";
 strcpy(value,s);return (int)strlen(s);
}
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t; va_list ap; va_start(ap,f); vsnprintf(stat_line,sizeof stat_line,f,ap); va_end(ap); return 0;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo*i,const VkAllocationCallbacks*a,VkFence*o){(void)d;(void)i;(void)a;*o=(VkFence)7;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks*a){(void)d;(void)f;(void)a;}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){(void)q;(void)f;assert(n==1&&i->commandBufferCount>0&&i->pCommandBuffers);submitted++;return (!strcmp(mode,"submit-fail")||!strcmp(mode,"prior-fail"))?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d,uint32_t n,const VkFence*f,VkBool32 all,uint64_t timeout) { (void)d;(void)n;(void)f;(void)all;(void)timeout; return !strcmp(mode,"timeout") ? VK_TIMEOUT : VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo*i,const VkAllocationCallbacks*a,VkImageView*o){(void)d;(void)i;(void)a;*o=(VkImageView)(uintptr_t)(0x1000+ ++view_new);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo*i,const VkAllocationCallbacks*a,VkRenderPass*o){(void)d;(void)i;(void)a;*o=(VkRenderPass)3;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo*i,const VkAllocationCallbacks*a,VkFramebuffer*o){(void)d;(void)i;(void)a;*o=(VkFramebuffer)(uintptr_t)(0x2000+ ++fb_new);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo*i,VkCommandBuffer*o){(void)d;(void)i;*o=(VkCommandBuffer)(uintptr_t)(0x3000+ ++cmds_new);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo*i){(void)c;(void)i;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){(void)c;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo*i,VkSubpassContents s){(void)c;(void)i;(void)s;}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c){(void)c;}
VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer c,uint32_t a,const VkClearAttachment*b,uint32_t n,const VkClearRect*r){(void)c;(void)a;(void)b;(void)n;(void)r;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer*c){(void)d;(void)p;assert(n==1);freed++;
 for(int i=0;i<g_fq.ncmd;i++)assert(g_fq.cmds[i]!=*c && "freed command remains owned by frame queue");
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks*a){(void)d;(void)f;(void)a;assert(completed || !strcmp(mode,"not-taken"));fb_freed++;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;assert(completed || !strcmp(mode,"not-taken"));view_freed++;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks*a){(void)d;(void)i;(void)a;assert(completed);image_freed++;}
// 解析器替身只回传该 serial/image 的已存在对象，不创建资源或补成功。
void *s2a_vk_adapter_clear_tex(uint64_t serial, VkImage image) {
 for (int i=0;i<25;i++) if (record_owners && record_owners[i].serial==serial && record_owners[i].image==image) return &record_owners[i];
 return NULL;
}
static int guest_read(void *o,uint64_t a,void *b,size_t n){(void)o;memcpy(b,(void*)(uintptr_t)a,n);return 0;}
static int guest_write(void *o,uint64_t a,const void *b,size_t n){(void)o;memcpy((void*)(uintptr_t)a,b,n);return 0;}
static size_t append_record(uint8_t *s,size_t pos,uint32_t op,const void *p,size_t n){
 size_t size=(sizeof(op_hdr)+n+7)&~(size_t)7;op_hdr h={op,(uint32_t)size};memcpy(s+pos,&h,sizeof h);
 if(p)memcpy(s+pos+sizeof h,p,n);return pos+size;
}
static void test_record_path(struct s2a_vk_device *d, int depth, struct nvn_vk_tex *owners) {
 uint8_t stream[32768]={0}; size_t pos=0; tex_t src[25]; struct s2a_replay_texture textures[25];
 memset(src,0,sizeof src); memset(textures,0,sizeof textures); record_owners=owners;
 for (int i=0;i<25;i++) {
  src[i].magic=0x00584554; src[i].d.format=depth?NVN_FORMAT_DEPTH32F:NVN_FORMAT_RGBA8;
  src[i].d.width=src[i].d.height=16; src[i].d.levels=1; src[i].serial=i+100;
  owners[i].valid=1; owners[i].image=(VkImage)(uintptr_t)(0x100+i); owners[i].generation=i+1; owners[i].serial=i+100;
  textures[i]=(struct s2a_replay_texture){.key=&src[i],.image=owners[i].image,
    .format=depth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM,.width=16,.height=16,.depth=1,.serial=src[i].serial,.levels=1,.layers=1};
  p_set_rt rt={0}; if(depth)rt.depth=&src[i];else {rt.n=1;rt.color[0]=&src[i];}
  size_t len=(sizeof(op_hdr)+set_rt_size(&rt)+7)&~(size_t)7;op_hdr h={OP_SET_RT,(uint32_t)len};
  memcpy(stream+pos,&h,sizeof h);set_rt_put(stream+pos+sizeof h,&rt);pos+=len;
  if(depth){p_clear_ds v={.depth=1,.depth_mask=1};pos=append_record(stream,pos,OP_CLEAR_DS,&v,sizeof v);}
  else {p_clear_color v={.index=0,.mask=15};pos=append_record(stream,pos,OP_CLEAR_COLOR,&v,sizeof v);}
 }
 pos=append_record(stream,pos,OP_END,NULL,0);
 struct s2a_record_memory memory={.read=guest_read,.write=guest_write}; struct s2a_record_result result;
 int rc=s2a_record_replay(d,stream,stream+pos,&memory,textures,25,NULL,0,NULL,0,&result);
 if(rc!=S2A_RECORD_OK) fprintf(stderr,"record failed %d: %s\n",rc,result.why);
 assert(rc==S2A_RECORD_OK && g_fq.ncmd==25 && g_pin_n==25);
 assert(g_fq.nclear==1);
 puts("vk_record 25 clear targets PASS");
}
int main(int argc, char **argv) {
 assert(argc == 3); mode=argv[1]; int depth=argv[2][0]=='d';
 struct s2a_vk_device d={0}; d.device=(VkDevice)1; d.queue=(VkQueue)1; d.command_pool=(VkCommandPool)1; g_fq.dev=&d;
 s2a_clear_target target={0}; target.width=16; target.height=16;
 target.format=depth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
 target.aspect=depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
 VkClearColorValue value={{0,0,0,1}}; char why[256]={0};
 static struct nvn_vk_tex owners[26];
 if (!strcmp(mode,"record")) { test_record_path(&d,depth,owners); return 0; }
 static struct nvn_vk_tex *slots[25];
 static struct nvn_vk_texcache cache; cache.d=&d; cache.slots=slots; cache.nslots=cache.cap=25;
 for (unsigned i=0;i<25;i++) {
  target.image=(VkImage)(uintptr_t)(0x100+i);
  owners[i].valid=1; owners[i].image=target.image; owners[i].generation=i+1; owners[i].serial=i+100; owners[i].owner=&cache; slots[i]=&owners[i]; target.native_tex=&owners[i];
  if (i==24 && !strcmp(mode,"not-taken")) g_fq.ncmd=fd_fq_cmd_capacity();
  int rc=depth?s2a_vk_clear_ds_queued(&d,&target,1.0f,0,1,0,NULL,why,sizeof why):s2a_vk_clear_color_queued(&d,&target,&value,15,NULL,why,sizeof why);
  if (i==24 && (!strcmp(mode,"off") || !strcmp(mode,"not-taken"))) {
   assert(rc==S2A_VK_CLEAR_UNHANDLED && owners[i].pins==0);
   if (!strcmp(mode,"not-taken")) assert(fb_freed==1 && view_freed==1);
   else assert(fb_new==24 && fb_freed==0 && g_pin_n==0);
   puts("fallback ownership PASS"); return 0;
  }
  if (i==24 && !strcmp(mode,"failed-owned")) {
   assert(rc==S2A_VK_CLEAR_FQ_FAILED && g_fq.state==FD_FQ_FAILED && g_fq.ncmd==25);
   assert(owners[i].pins==1 && g_fq.nclear==1 && fb_freed==0 && view_freed==0);
   puts("failed-owned retains command, framebuffer, view and image pin PASS"); return 0;
  }
  if (rc!=S2A_VK_CLEAR_QUEUED) { fprintf(stderr,"FAIL: target %u expected queued, got %d (%s)\n",i+1,rc,why); return 1; }
 }
 assert(g_fq.ncmd==25 && g_pin_n==25 && g_fq.nclear==1);
 if (!strcmp(mode,"timeout")) {
  static struct fd_slot pending; pending.used=1; pending.q=g_fq;
  pending.q.nbatch_fence=1; pending.q.batch_fence[0]=(VkFence)7;
  assert(fd_slot_retire(&d,&pending)==-1);
  assert(pending.used && owners[0].pins==1 && owners[24].pins==1 && fb_freed==0 && view_freed==0);
  puts("timeout keeps resources and pins PASS"); return 0;
 }
 for (unsigned i=0;i<25;i++) assert(owners[i].pins==1);
 assert(fb_new==25 && view_new==25 && fb_freed==0 && view_freed==0);
 s2a_vk_clear_fv_present();
 assert(strstr(stat_line,"unique=25 overflow=0 created=25 temporary=1 hits=0"));
 // Real texcache finalize while in flight: pin keeps the image and its cache entry alive.
 VkImage old_image=owners[0].image; uint64_t old_generation=owners[0].generation;
 tex_t guest={0}; guest.serial=owners[0].serial;
 nvn_vk_texcache_drop_tex(&cache, &guest);
 assert(owners[0].retired && owners[0].pins==1 && image_freed==0);
 s2a_vk_clear_forget_image(&d, old_image, old_generation);
 assert(fb_freed==0 && view_freed==0);
 // Model a queue slot only after its fence is known complete; use production slot destructor.
 static struct fd_frame_q done; done=g_fq; fd_fq_reset(); completed=1;
 fd_slot_release(&d,&done); fd_unpin_all();
 assert(fb_freed==2 && view_freed==2 && image_freed==1 && !owners[0].valid);
 for (unsigned i=0;i<25;i++) assert(owners[i].pins==0);
 // The old VkImage has really been destroyed; simulate the driver reusing the handle.
 owners[25].valid=1; owners[25].image=old_image; owners[25].generation=100;
 target.image=owners[25].image; target.native_tex=&owners[25];
 g_fq.dev=&d;
 int rc=depth?s2a_vk_clear_ds_queued(&d,&target,1.0f,0,1,0,NULL,why,sizeof why):s2a_vk_clear_color_queued(&d,&target,&value,15,NULL,why,sizeof why);
 assert(rc==S2A_VK_CLEAR_QUEUED && fb_new==26 && view_new==26 && owners[25].pins==1);
 assert(fb_freed==2 && view_freed==2 && owners[25].pins==1);
 puts("full, in-flight, retire and image-handle reuse PASS");return 0;
}
