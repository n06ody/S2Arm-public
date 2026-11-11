// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real clear + frame queue with lavapipe and synchronization validation.
#include "../native/nvn/nvn_vk_first_draw.c"
#include "nvn_vk_session.h"
#include "vk_clear.h"
#include <assert.h>
#include <stdarg.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 2; } } while (0)
int __android_log_print(int p,const char *tag,const char *fmt,...){(void)p;(void)tag;(void)fmt;return 0;}
int __system_property_get(const char *name,char *value) {
    const char *s = !strcmp(name,"debug.s2a.clear_fv_q") ? "1" :
                    !strcmp(name,"debug.s2a.batch_passes") ? "512" : "";
    strcpy(value,s); return (int)strlen(s);
}
uint64_t s2a_vk_adapter_registry_generation(void) { return 1; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) {(void)w;return 0;}
int32_t ANativeWindow_getHeight(ANativeWindow *w) {(void)w;return 0;}
int main(void) {
    uint32_t nl=0; CHECK(vkEnumerateInstanceLayerProperties(&nl,NULL)==VK_SUCCESS && nl);
    VkLayerProperties *layers=calloc(nl,sizeof *layers); CHECK(layers);
    CHECK(vkEnumerateInstanceLayerProperties(&nl,layers)==VK_SUCCESS);
    int found=0; for (uint32_t i=0;i<nl;i++) if (!strcmp(layers[i].layerName,"VK_LAYER_KHRONOS_validation")) found=1;
    free(layers); CHECK(found && getenv("VK_INSTANCE_LAYERS") && strstr(getenv("VK_INSTANCE_LAYERS"),"VK_LAYER_KHRONOS_validation"));
    struct nvn_vk_session session={0}; char why[256]={0};
    CHECK(nvn_vk_session_create(&session,why,sizeof why)==0);
    struct s2a_vk_device *d=&session.device;
    CHECK(d->properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU && strstr(d->properties.deviceName,"llvmpipe"));
    fprintf(stderr,"device=%s validation=enabled sync_validation=enabled\n",d->properties.deviceName);
    VkImage images[25]={0}; VkDeviceMemory mem[25]={0}; static struct nvn_vk_tex tex[25];
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={4,4,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    for (int i=0;i<25;i++) {
        CHECK(vkCreateImage(d->device,&ii,NULL,&images[i])==VK_SUCCESS);
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(d->device,images[i],&mr);
        uint32_t type=s2a_vk_memory_type(d,mr.memoryTypeBits,0,NULL); CHECK(type!=UINT32_MAX);
        VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=type};
        CHECK(vkAllocateMemory(d->device,&ma,NULL,&mem[i])==VK_SUCCESS);
        CHECK(vkBindImageMemory(d->device,images[i],mem[i],0)==VK_SUCCESS);
        tex[i].valid=1;tex[i].image=images[i];tex[i].generation=(uint64_t)i+1;
    }
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=d->command_pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer init=VK_NULL_HANDLE; CHECK(vkAllocateCommandBuffers(d->device,&ca,&init)==VK_SUCCESS);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(init,&bi)==VK_SUCCESS);
    for (int i=0;i<25;i++) {
        VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        vkCmdPipelineBarrier(init,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&b);
    }
    CHECK(vkEndCommandBuffer(init)==VK_SUCCESS);
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence=VK_NULL_HANDLE;
    CHECK(vkCreateFence(d->device,&fi,NULL,&fence)==VK_SUCCESS);
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&init};
    CHECK(vkQueueSubmit(d->queue,1,&si,fence)==VK_SUCCESS);
    CHECK(vkWaitForFences(d->device,1,&fence,VK_TRUE,5000000000ull)==VK_SUCCESS);
    vkDestroyFence(d->device,fence,NULL); vkFreeCommandBuffers(d->device,d->command_pool,1,&init);
    g_fq.dev=d; g_fq.dev_handle=d->device;
    VkClearColorValue color={{0.25f,0.5f,0.75f,1.0f}};
    for (int i=0;i<25;i++) {
        s2a_clear_target target={.image=images[i],.format=VK_FORMAT_R8G8B8A8_UNORM,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,
                                 .width=4,.height=4,.native_tex=&tex[i]};
        CHECK(s2a_vk_clear_color_queued(d,&target,&color,15,NULL,why,sizeof why)==S2A_VK_CLEAR_QUEUED);
    }
    CHECK(g_fq.ncmd==25 && g_fq.nclear==1 && g_pin_n==25);
    CHECK(nvn_vk_first_draw_flush()==0);
    // Read one pixel from each image; validate the clear really executed, not only object lifetime.
    VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory hostmem=VK_NULL_HANDLE;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=25u*4u,
                           .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(d->device,&bc,NULL,&buffer)==VK_SUCCESS);
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(d->device,buffer,&br);
    uint32_t bt=s2a_vk_memory_type(d,br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,NULL);
    CHECK(bt!=UINT32_MAX);
    VkMemoryAllocateInfo ba={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=br.size,.memoryTypeIndex=bt};
    CHECK(vkAllocateMemory(d->device,&ba,NULL,&hostmem)==VK_SUCCESS);
    CHECK(vkBindBufferMemory(d->device,buffer,hostmem,0)==VK_SUCCESS);
    VkCommandBuffer readback=VK_NULL_HANDLE; CHECK(vkAllocateCommandBuffers(d->device,&ca,&readback)==VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(readback,&bi)==VK_SUCCESS);
    for (int i=0;i<25;i++) {
        VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,
          .newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
          .image=images[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
          .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(readback,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
        VkBufferImageCopy copy={.bufferOffset=(VkDeviceSize)i*4u,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={1,1,1}};
        vkCmdCopyImageToBuffer(readback,images[i],VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
    }
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
      .buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(readback,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    CHECK(vkEndCommandBuffer(readback)==VK_SUCCESS);
    CHECK(vkCreateFence(d->device,&fi,NULL,&fence)==VK_SUCCESS);
    VkSubmitInfo read_si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&readback};
    CHECK(vkQueueSubmit(d->queue,1,&read_si,fence)==VK_SUCCESS);
    CHECK(vkWaitForFences(d->device,1,&fence,VK_TRUE,5000000000ull)==VK_SUCCESS);
    void *mapped=NULL; CHECK(vkMapMemory(d->device,hostmem,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    for (int i=0;i<25;i++) {
        const uint8_t *px=(const uint8_t *)mapped+i*4;
        CHECK(px[0]==64 && px[1]==128 && px[2]==191 && px[3]==255);
    }
    vkUnmapMemory(d->device,hostmem); vkDestroyFence(d->device,fence,NULL);
    vkFreeCommandBuffers(d->device,d->command_pool,1,&readback); vkDestroyBuffer(d->device,buffer,NULL);
    vkFreeMemory(d->device,hostmem,NULL);
    nvn_vk_first_draw_present_done();
    for (int i=0;i<25;i++) { CHECK(tex[i].pins==0); s2a_vk_clear_forget_image(d,images[i],tex[i].generation); }
    for (int i=0;i<25;i++) { vkDestroyImage(d->device,images[i],NULL); vkFreeMemory(d->device,mem[i],NULL); }
    CHECK(nvn_vk_session_destroy_with_callback(&session,nvn_vk_first_draw_release_device)==0);
    fprintf(stderr,"PASS: 25 real clear targets, RGBA readback, queue retirement and session teardown\n");
    return 0;
}
