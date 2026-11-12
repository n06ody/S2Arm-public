// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Production texture cache with real Vulkan; intercept image/view creation to
// check both the default stencil view and an explicit depth-to-stencil view.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_vk_texcache.h"
#include "nvn_vk_format.h"
#include "nvn_tex.h"
#include "include/nvn.h"

static VkImage image;
static VkFormat image_format;
static VkImageCreateFlags image_flags;
static int stencil_views, wrong_views;
static unsigned char storage[65536];
static uint32_t *shader_code;
static size_t shader_size;
int __android_log_print(int p,const char *t,const char *f,...){(void)p;(void)t;(void)f;return 0;}
int __system_property_get(const char *n,char *v){(void)n;*v=0;return 0;}
void nvn_vk_first_draw_forget_view(VkImageView v){(void)v;}
void dm_fault_hook_register(int (*f)(void *,int,void *)){(void)f;}
void dm_guest_write_prep_register(void (*f)(void *,size_t)){(void)f;}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateImage(VkDevice,const VkImageCreateInfo *,const VkAllocationCallbacks *,VkImage *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImage(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out){
    VkResult rc=__real_vkCreateImage(d,ci,a,out);
    if(rc==VK_SUCCESS){image=*out;image_format=ci->format;image_flags=ci->flags;}
    return rc;
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateImageView(VkDevice,const VkImageViewCreateInfo *,const VkAllocationCallbacks *,VkImageView *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *ci,const VkAllocationCallbacks *a,VkImageView *out){
    if(ci->image==image && ci->subresourceRange.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT){
        stencil_views++;
        if(ci->format!=image_format || (image_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ||
           ci->components.r!=VK_COMPONENT_SWIZZLE_IDENTITY || ci->components.g!=VK_COMPONENT_SWIZZLE_IDENTITY ||
           ci->components.b!=VK_COMPONENT_SWIZZLE_IDENTITY || ci->components.a!=VK_COMPONENT_SWIZZLE_IDENTITY){
            fprintf(stderr,"FAIL stencil view image.format=%d view.format=%d aspect=%u flags=%u components=%d,%d,%d,%d\n",
                (int)image_format,(int)ci->format,(unsigned)ci->subresourceRange.aspectMask,(unsigned)image_flags,
                ci->components.r,ci->components.g,ci->components.b,ci->components.a);
            wrong_views++;
        }
    }
    return __real_vkCreateImageView(d,ci,a,out);
}

// Clear the stencil aspect to a known value, then sample the production view
// through an unsigned sampler and read all four returned components on the CPU.
static int sample_stencil(struct s2a_vk_device *d,VkImage img,VkImageView view){
    VkDevice dev=d->device; int rc=1; void *mapped=NULL;
    VkBuffer buf=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE;
    VkDescriptorSetLayout dl=VK_NULL_HANDLE; VkPipelineLayout pl=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE; VkSampler sampler=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE; VkPipeline pipe=VK_NULL_HANDLE;
    VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
#define VK_OK(call) do { VkResult e=(call); if(e!=VK_SUCCESS){fprintf(stderr,"FAIL %s rc=%d\n",#call,(int)e);goto done;} } while(0)
    VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=16; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VK_OK(vkCreateBuffer(dev,&bi,NULL,&buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,buf,&mr);
    VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize=mr.size;
    ma.memoryTypeIndex=s2a_vk_memory_type(d,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,NULL);
    if(ma.memoryTypeIndex==UINT32_MAX){fprintf(stderr,"FAIL no coherent host memory\n");goto done;}
    VK_OK(vkAllocateMemory(dev,&ma,NULL,&mem)); VK_OK(vkBindBufferMemory(dev,buf,mem,0));
    VK_OK(vkMapMemory(dev,mem,0,16,0,&mapped)); memset(mapped,0,16);
    VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
                                             {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; di.bindingCount=2; di.pBindings=bindings;
    VK_OK(vkCreateDescriptorSetLayout(dev,&di,NULL,&dl));
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount=1; li.pSetLayouts=&dl;
    VK_OK(vkCreatePipelineLayout(dev,&li,NULL,&pl));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=2; pi.pPoolSizes=sizes;
    VK_OK(vkCreateDescriptorPool(dev,&pi,NULL,&pool));
    VkDescriptorSetAllocateInfo dai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool=pool; dai.descriptorSetCount=1; dai.pSetLayouts=&dl;
    VkDescriptorSet set; VK_OK(vkAllocateDescriptorSets(dev,&dai,&set));
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.borderColor=VK_BORDER_COLOR_INT_TRANSPARENT_BLACK; si.maxLod=0;
    VK_OK(vkCreateSampler(dev,&si,NULL,&sampler));
    VkDescriptorImageInfo ii={sampler,view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo db={buf,0,16};
    VkWriteDescriptorSet writes[2]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet=set; writes[0].dstBinding=0; writes[0].descriptorCount=1; writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&ii;
    writes[1].dstSet=set; writes[1].dstBinding=1; writes[1].descriptorCount=1; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&db;
    vkUpdateDescriptorSets(dev,2,writes,0,NULL);
    VkShaderModuleCreateInfo sm={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=shader_size; sm.pCode=shader_code;
    VK_OK(vkCreateShaderModule(dev,&sm,NULL,&shader));
    VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout=pl;
    ci.stage=(VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",NULL};
    VK_OK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,NULL,&pipe));
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=d->command_pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VK_OK(vkAllocateCommandBuffers(dev,&ca,&cmd));
    VkCommandBufferBeginInfo cb={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_OK(vkBeginCommandBuffer(cmd,&cb));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=img; b.subresourceRange=range;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkClearDepthStencilValue clear={0.0f,0x5a}; vkCmdClearDepthStencilImage(cmd,img,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&b);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,NULL); vkCmdDispatch(cmd,1,1,1);
    VkBufferMemoryBarrier bb={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; bb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; bb.buffer=buf; bb.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&bb,0,NULL);
    VK_OK(vkEndCommandBuffer(cmd)); VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_OK(vkCreateFence(dev,&fi,NULL,&fence));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    VK_OK(vkQueueSubmit(d->queue,1,&submit,fence)); VK_OK(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX));
    uint32_t *v=mapped; printf("usampler RGBA=%u,%u,%u,%u expected R=90\n",v[0],v[1],v[2],v[3]); rc=v[0]!=0x5a;
done:
    if(fence) vkDestroyFence(dev,fence,NULL); if(cmd) vkFreeCommandBuffers(dev,d->command_pool,1,&cmd);
    if(pipe) vkDestroyPipeline(dev,pipe,NULL); if(shader) vkDestroyShaderModule(dev,shader,NULL);
    if(pool) vkDestroyDescriptorPool(dev,pool,NULL); if(sampler) vkDestroySampler(dev,sampler,NULL);
    if(pl) vkDestroyPipelineLayout(dev,pl,NULL); if(dl) vkDestroyDescriptorSetLayout(dev,dl,NULL);
    if(mapped) vkUnmapMemory(dev,mem); if(buf) vkDestroyBuffer(dev,buf,NULL); if(mem) vkFreeMemory(dev,mem,NULL);
#undef VK_OK
    return rc;
}

static int run_case(struct s2a_vk_device *d,uint32_t format,int default_stencil){
    struct nvn_vk_texcache c; char why[256]={0};
    if(nvn_vk_texcache_init(&c,d,why,sizeof why)){fprintf(stderr,"FAIL cache init: %s\n",why);return 1;}
    tex_t t={0}; t.magic=0x54584554; t.mem=storage; t.serial=100+format*2+(unsigned)default_stencil;
    t.d.target=1; t.d.format=format; t.d.width=2; t.d.height=2; t.d.depth=1; t.d.levels=1;
    t.stencil=default_stencil; nvn_tex_layout(&t.d,&t.L);
    image=VK_NULL_HANDLE; int start=stencil_views, bad=wrong_views;
    struct nvn_vk_tex *tex=nvn_vk_texcache_get_tex(&c,&t,why,sizeof why);
    if(!tex){fprintf(stderr,"FAIL get_tex format=%u stencil=%d: %s\n",format,default_stencil,why);nvn_vk_texcache_destroy(&c);return 1;}
    VkImageView view=tex->view;
    if(!default_stencil){
        view_t v={0}; v.set_dsmode=1; v.dsmode=1;
        view=nvn_vk_texcache_view(&c,tex,&v,why,sizeof why);
        if(!view){
            fprintf(stderr,"FAIL explicit stencil view format=%u: %s\n",format,why);nvn_vk_texcache_destroy(&c);return 1;
        }
    }
    int ok=stencil_views==start+1 && wrong_views==bad;
    if(ok && sample_stencil(d,tex->image,view)) ok=0;
    printf("%s format=%u path=%s stencil_views=%d wrong=%d\n",ok?"PASS":"FAIL",format,
           default_stencil?"default":"explicit",stencil_views-start,wrong_views-bad);
    nvn_vk_texcache_destroy(&c);
    return !ok;
}
int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"FAIL missing shader\n");return 1;}
    FILE *f=fopen(argv[1],"rb"); if(!f || fseek(f,0,SEEK_END)){fprintf(stderr,"FAIL shader open\n");return 1;}
    long size=ftell(f); if(size<=0 || size%4 || fseek(f,0,SEEK_SET)){fclose(f);return 1;}
    shader_size=(size_t)size; shader_code=malloc(shader_size);
    if(!shader_code || fread(shader_code,1,shader_size,f)!=shader_size){fclose(f);free(shader_code);return 1;} fclose(f);
    struct s2a_vk_device d; char why[256]={0};
    if(s2a_vk_device_create(&d,why,sizeof why)){fprintf(stderr,"FAIL device: %s\n",why);return 1;}
    const uint32_t formats[]={NVN_FORMAT_DEPTH24_STENCIL8,NVN_FORMAT_DEPTH32F_STENCIL8};
    int failed=0;
    for(unsigned i=0;i<sizeof formats/sizeof formats[0];i++){
        failed+=run_case(&d,formats[i],1);
        failed+=run_case(&d,formats[i],0);
    }
    s2a_vk_device_destroy(&d,0);
    free(shader_code);
    printf("stencil view cases=%u failed=%d\n",4u,failed);
    return failed?1:0;
}
