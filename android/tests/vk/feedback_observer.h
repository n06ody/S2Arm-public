// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Observe actual Vulkan resource identities. Names and types come from NDK 29;
// no backend structs or invented Vulkan declarations are used here.
#include <pthread.h>
#include <stdatomic.h>
static pthread_mutex_t observed_mu = PTHREAD_MUTEX_INITIALIZER;
#define OBS_REAL(f) extern __typeof__(f) __real_##f
#define OBS_TYPE(f) _Static_assert(__builtin_types_compatible_p(__typeof__(&__wrap_##f), PFN_##f), #f " signature")
static struct { VkImageView view; VkImage image; VkImageSubresourceRange range; unsigned char dead; } observed_views[256];   // dead：已销毁（同一个句柄重新创建时清零）
static struct { VkFramebuffer fb; VkImageView views[9]; unsigned n; int imageless; } observed_fbs[256];
static struct { VkDescriptorSet set; VkImageView views[4][8]; } observed_sets[256];
static unsigned observed_nv, observed_nf, observed_ns, observed_draws, observed_overlap, observed_copies;
static unsigned observed_dead_samples;   // 绘制时绑着的组里被采样的、已经销毁的视图条数（整池组回收：补丁没还原时才会出现）
static unsigned observed_expect_samples=1, observed_early_destroy;
static VkImage observed_copy_targets[32];
static unsigned observed_ntargets;
static _Atomic int observed_fence_proven;
static int observed_aborted;
static VkFramebuffer observed_fb;
static VkDescriptorSet observed_bound[16];
// Optional observers for the read-only extension fixture; the original
// snapshot tests leave them NULL and keep their existing alias assertions.
static void (*observed_on_begin)(const VkRenderPassBeginInfo *);
static void (*observed_on_draw)(void);
static unsigned (*observed_sample_sets)(void);
static PFN_vkVoidFunction (*observed_command)(const char *);
static void observer_reset(void) {
    observed_nv=observed_nf=observed_ns=observed_draws=observed_overlap=observed_copies=observed_dead_samples=0;
    observed_ntargets=observed_early_destroy=0;observed_expect_samples=1;observed_aborted=0;atomic_store(&observed_fence_proven,0);
    observed_fb=VK_NULL_HANDLE;memset(observed_bound,0,sizeof observed_bound);
}
static unsigned observed_view(VkImageView v) {
    for(unsigned i=0;i<observed_nv;i++)if(observed_views[i].view==v)return i;
    REQUIRE(0);return 0;
}
static unsigned observed_set(VkDescriptorSet s) {
    for(unsigned i=0;i<observed_ns;i++)if(observed_sets[i].set==s)return i;
    REQUIRE(observed_ns<256);unsigned i=observed_ns++;observed_sets[i].set=s;memset(observed_sets[i].views,0,sizeof observed_sets[i].views);return i;
}
OBS_REAL(vkCreateImageView);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *p,const VkAllocationCallbacks *a,VkImageView *v) {
    VkResult r=__real_vkCreateImageView(d,p,a,v);
    pthread_mutex_lock(&observed_mu);
    if(r==VK_SUCCESS){unsigned i=0;while(i<observed_nv&&observed_views[i].view!=*v)i++;if(i==observed_nv){REQUIRE(observed_nv<256);observed_nv++;}observed_views[i].view=*v;observed_views[i].image=p->image;observed_views[i].range=p->subresourceRange;observed_views[i].dead=0;}
    pthread_mutex_unlock(&observed_mu);return r;
}
OBS_TYPE(vkCreateImageView);
OBS_REAL(vkCreateFramebuffer);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo *p,const VkAllocationCallbacks *a,VkFramebuffer *f) {
    VkResult r=__real_vkCreateFramebuffer(d,p,a,f);
    pthread_mutex_lock(&observed_mu);
    if (r == VK_SUCCESS) {
        REQUIRE(p->attachmentCount <= 9);
        unsigned i = 0;
        while (i < observed_nf && observed_fbs[i].fb != *f) i++;
        if (i == observed_nf) { REQUIRE(observed_nf < 256); observed_nf++; }
        observed_fbs[i].fb = *f;
        observed_fbs[i].n = p->attachmentCount;
        observed_fbs[i].imageless = (p->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT_KHR) != 0;
        if (p->attachmentCount && !observed_fbs[i].imageless)
            memcpy(observed_fbs[i].views, p->pAttachments, p->attachmentCount * sizeof(VkImageView));
    }
    pthread_mutex_unlock(&observed_mu);return r;
}
OBS_TYPE(vkCreateFramebuffer);
OBS_REAL(vkCmdBeginRenderPass);
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *p,VkSubpassContents s) {
    pthread_mutex_lock(&observed_mu);
    observed_fb = p->framebuffer;
    unsigned i = 0;
    while (i < observed_nf && observed_fbs[i].fb != observed_fb) i++;
    if (i < observed_nf && observed_fbs[i].imageless) {
        // 无图帧缓冲在每次开通道时才绑定这次实际使用的视图。
        const VkBaseInStructure *chain = p->pNext;
        while (chain && chain->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO_KHR)
            chain = chain->pNext;
        REQUIRE(chain);
        const VkRenderPassAttachmentBeginInfoKHR *attachments = (const VkRenderPassAttachmentBeginInfoKHR *)chain;
        REQUIRE(attachments->attachmentCount == observed_fbs[i].n);
        if (attachments->attachmentCount) {
            REQUIRE(attachments->pAttachments);
            memcpy(observed_fbs[i].views, attachments->pAttachments, attachments->attachmentCount * sizeof(VkImageView));
        }
    }
    pthread_mutex_unlock(&observed_mu);
    if (observed_on_begin) observed_on_begin(p);
    __real_vkCmdBeginRenderPass(c,p,s);
}
OBS_TYPE(vkCmdBeginRenderPass);
OBS_REAL(vkUpdateDescriptorSets);
VKAPI_ATTR void VKAPI_CALL __wrap_vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t nc,const VkCopyDescriptorSet *cp) {
    pthread_mutex_lock(&observed_mu);
    for(uint32_t i=0;i<n;i++)if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){REQUIRE(w[i].dstBinding<4&&w[i].dstArrayElement+w[i].descriptorCount<=8);unsigned si=observed_set(w[i].dstSet);for(uint32_t k=0;k<w[i].descriptorCount;k++)observed_sets[si].views[w[i].dstBinding][w[i].dstArrayElement+k]=w[i].pImageInfo[k].imageView;}
    for(uint32_t i=0;i<nc;i++){REQUIRE(cp[i].srcBinding<4&&cp[i].dstBinding<4&&cp[i].srcArrayElement+cp[i].descriptorCount<=8&&cp[i].dstArrayElement+cp[i].descriptorCount<=8);unsigned src=observed_set(cp[i].srcSet),dst=observed_set(cp[i].dstSet);for(uint32_t k=0;k<cp[i].descriptorCount;k++)observed_sets[dst].views[cp[i].dstBinding][cp[i].dstArrayElement+k]=observed_sets[src].views[cp[i].srcBinding][cp[i].srcArrayElement+k];}
    pthread_mutex_unlock(&observed_mu);__real_vkUpdateDescriptorSets(d,n,w,nc,cp);
}
OBS_TYPE(vkUpdateDescriptorSets);
OBS_REAL(vkCmdBindDescriptorSets);
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t no,const uint32_t *o) {
    if(b==VK_PIPELINE_BIND_POINT_GRAPHICS){REQUIRE(first+n<=16);for(uint32_t i=0;i<n;i++)observed_bound[first+i]=s[i];}
    __real_vkCmdBindDescriptorSets(c,b,l,first,n,s,no,o);
}
OBS_TYPE(vkCmdBindDescriptorSets);
static int observed_intersects(VkImageSubresourceRange a,VkImageSubresourceRange b) {
    return (a.aspectMask&b.aspectMask) && (uint64_t)a.baseMipLevel+a.levelCount>b.baseMipLevel &&
        (uint64_t)b.baseMipLevel+b.levelCount>a.baseMipLevel &&
        (uint64_t)a.baseArrayLayer+a.layerCount>b.baseArrayLayer &&
        (uint64_t)b.baseArrayLayer+b.layerCount>a.baseArrayLayer;
}
OBS_REAL(vkCmdDraw);
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDraw(VkCommandBuffer c,uint32_t n,uint32_t instances,uint32_t first,uint32_t base) {
    pthread_mutex_lock(&observed_mu);
    unsigned f=0;while(f<observed_nf&&observed_fbs[f].fb!=observed_fb)f++;REQUIRE(f<observed_nf);
    unsigned samples=0;
    unsigned nsets=observed_sample_sets?observed_sample_sets():16;
    for(unsigned i=0;i<nsets;i++)if(observed_bound[i]){
      unsigned si=observed_set(observed_bound[i]);
      for(unsigned binding=0;binding<4;binding++)for(unsigned elem=0;elem<8;elem++){
        VkImageView v=observed_sets[si].views[binding][elem];if(!v)continue;
        unsigned s=observed_view(v);samples++;if(observed_views[s].dead)observed_dead_samples++;
        for(unsigned k=0;k<observed_fbs[f].n;k++){
            unsigned a=observed_view(observed_fbs[f].views[k]);
            if(observed_views[a].image==observed_views[s].image&&observed_intersects(observed_views[a].range,observed_views[s].range))observed_overlap++;
        }
      }
    }
    REQUIRE(samples==(nsets?observed_expect_samples:0));if(observed_on_draw)observed_on_draw();observed_draws++;pthread_mutex_unlock(&observed_mu);__real_vkCmdDraw(c,n,instances,first,base);
}
OBS_TYPE(vkCmdDraw);
OBS_REAL(vkCmdCopyImage);
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdCopyImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy *r) {
    pthread_mutex_lock(&observed_mu);observed_copies++;
    unsigned i=0;while(i<observed_ntargets&&observed_copy_targets[i]!=dst)i++;
    if(i==observed_ntargets){REQUIRE(i<32);observed_copy_targets[observed_ntargets++]=dst;}
    pthread_mutex_unlock(&observed_mu);__real_vkCmdCopyImage(c,src,sl,dst,dl,n,r);
}
OBS_TYPE(vkCmdCopyImage);
OBS_REAL(vkDestroyImageView);
VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks *a) {
    pthread_mutex_lock(&observed_mu);
    unsigned vi=observed_view(v);observed_views[vi].dead=1;
    for(unsigned i=0;i<observed_ntargets;i++)if(observed_views[vi].image==observed_copy_targets[i]&&!atomic_load(&observed_fence_proven)&&!observed_aborted)observed_early_destroy++;
    pthread_mutex_unlock(&observed_mu);__real_vkDestroyImageView(d,v,a);
}
OBS_TYPE(vkDestroyImageView);
OBS_REAL(vkWaitForFences);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
    VkResult r=__real_vkWaitForFences(d,n,f,all,timeout);
    if(r==VK_SUCCESS)atomic_store(&observed_fence_proven,1);
    return r;
}
OBS_TYPE(vkWaitForFences);
// The backend DQ obtains command entry points dynamically even with DQ off.
// Interpose the real entry lookup too, preserving NULL for unsupported commands.
OBS_REAL(vkGetDeviceProcAddr);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d,const char *name) {
    PFN_vkVoidFunction real=__real_vkGetDeviceProcAddr(d,name);
    if(!real)return NULL;
    if(observed_command){PFN_vkVoidFunction hook=observed_command(name);if(hook)return hook;}
#define OBS_PROC(f) if(!strcmp(name,#f))return (PFN_vkVoidFunction)__wrap_##f
    OBS_PROC(vkCmdBeginRenderPass);OBS_PROC(vkCmdBindDescriptorSets);
    OBS_PROC(vkCmdDraw);OBS_PROC(vkCmdCopyImage);
#undef OBS_PROC
    return real;
}
OBS_TYPE(vkGetDeviceProcAddr);
#undef OBS_REAL
#undef OBS_TYPE
