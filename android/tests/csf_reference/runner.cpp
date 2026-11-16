// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Private snapshot executor. Compile against the pinned NDK Vulkan headers, never a fixture ABI.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define VK(call) do { VkResult r = (call); if (r != VK_SUCCESS) throw std::runtime_error(#call " rc=" + std::to_string(r)); } while (0)
static void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static std::vector<unsigned char> read(const std::string &path) {
    std::ifstream f(path, std::ios::binary); require(bool(f), "input missing");
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct Buffer { VkBuffer b{}; VkDeviceMemory m{}; void *cpu{}; uint64_t size{}; };
struct Binding { uint32_t set{}, binding{}, type{}, access{}; std::string alias, path; uint64_t off{}, len{}; };
struct Image { uint32_t set{}, binding{}, format{}, aspect{}, bpp{}, w{}, h{}, levels{}, sw[4]{}; std::string path; VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkSampler sampler{}; Buffer upload; };
struct Device {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkPhysicalDeviceProperties props{}; VkPhysicalDeviceMemoryProperties mem{}; uint32_t family{};
    Device() {
        const char *layer="VK_LAYER_KHRONOS_validation";
        uint32_t count=0; VK(vkEnumerateInstanceLayerProperties(&count,nullptr)); std::vector<VkLayerProperties> layers(count);
        VK(vkEnumerateInstanceLayerProperties(&count,layers.data()));
        require(std::any_of(layers.begin(),layers.end(),[&](auto &x){return !strcmp(x.layerName,layer);}),"validation missing");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="csf-reference"; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app; ci.enabledLayerCount=1; ci.ppEnabledLayerNames=&layer;
        VK(vkCreateInstance(&ci,nullptr,&instance)); VK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); require(count>0,"no device");
        std::vector<VkPhysicalDevice> devices(count); VK(vkEnumeratePhysicalDevices(instance,&count,devices.data())); physical=devices[0];
        vkGetPhysicalDeviceProperties(physical,&props); vkGetPhysicalDeviceMemoryProperties(physical,&mem);
        std::cout<<"device="<<props.deviceName<<" shared="<<props.limits.maxComputeSharedMemorySize<<" invocations="<<props.limits.maxComputeWorkGroupInvocations<<"\n";
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> qs(count); vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,qs.data());
        for (family=0;family<count && !(qs[family].queueFlags&VK_QUEUE_COMPUTE_BIT);family++) {}
        require(family<count,"no compute queue"); float priority=1;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=family; qc.queueCount=1; qc.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(physical,&features); features.robustBufferAccess=VK_FALSE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qc; dc.pEnabledFeatures=&features;
        VK(vkCreateDevice(physical,&dc,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
    }
    uint32_t memory(uint32_t bits,VkMemoryPropertyFlags flags) {
        for (uint32_t i=0;i<mem.memoryTypeCount;i++) if ((bits&(1u<<i)) && (mem.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("memory properties unavailable");
    }
    Buffer buffer(uint64_t bytes) {
        require(bytes>0 && bytes<=(512ull<<20),"buffer size invalid"); Buffer b; b.size=bytes;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size=bytes; ci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK(vkCreateBuffer(device,&ci,nullptr,&b.b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b.b,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memory(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(device,&ai,nullptr,&b.m)); VK(vkBindBufferMemory(device,b.b,b.m,0)); VK(vkMapMemory(device,b.m,0,VK_WHOLE_SIZE,0,&b.cpu)); memset(b.cpu,0,bytes); return b;
    }
    void release(Buffer &b) { if(b.cpu)vkUnmapMemory(device,b.m); if(b.b)vkDestroyBuffer(device,b.b,nullptr); if(b.m)vkFreeMemory(device,b.m,nullptr); }
    ~Device(){if(device)vkDestroyDevice(device,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}
};
int main(int argc,char **argv) try {
    require(argc==5,"usage: runner shader.spv input.spec output-dir required-shared-bytes");
    Device d; require(d.props.limits.maxComputeSharedMemorySize>=std::stoul(argv[4]),"reference shared-memory limit insufficient");
    std::ifstream spec(argv[2]); require(bool(spec),"missing spec"); std::string line; uint32_t dims[3]={}, local[3]={};
    std::vector<Binding> bindings; std::vector<Image> images; std::map<std::string,Buffer> buffers;
    while(std::getline(spec,line)) {
        std::istringstream s(line); std::string kind; s>>kind;
        if(kind=="groups") s>>dims[0]>>dims[1]>>dims[2];
        else if(kind=="local") s>>local[0]>>local[1]>>local[2];
        else if(kind=="buffer") { Binding b; s>>b.set>>b.binding>>b.type>>b.access>>b.alias>>b.off>>b.len>>b.path; require(bool(s),"bad buffer record"); bindings.push_back(b); }
        else if(kind=="image") { Image i; s>>i.set>>i.binding>>i.format>>i.aspect>>i.bpp>>i.w>>i.h>>i.levels>>i.sw[0]>>i.sw[1]>>i.sw[2]>>i.sw[3]>>i.path; require(bool(s),"bad image record"); images.push_back(i); }
        else require(false,"unknown spec record");
    }
    for(int i=0;i<3;i++) require(dims[i]>0 && dims[i]<=d.props.limits.maxComputeWorkGroupCount[i] && local[i]>0 && local[i]<=d.props.limits.maxComputeWorkGroupSize[i],"dispatch/local limit");
    require(uint64_t(local[0])*local[1]*local[2]<=d.props.limits.maxComputeWorkGroupInvocations,"local invocation limit");
    std::map<std::string,uint64_t> sizes;
    for(auto &b:bindings) { require(b.type<=1 && b.set<4 && b.len>0 && b.off<= (512ull<<20)-b.len,"invalid binding"); sizes[b.alias]=std::max(sizes[b.alias],b.off+b.len); }
    for(auto &s:sizes) buffers[s.first]=d.buffer(s.second);
    // The Python preflight proves overlapping snapshots agree. Preserve real descriptor offsets/aliases.
    for(auto &b:bindings) {auto bytes=read(b.path); require(bytes.size()==b.len,"input length mismatch"); memcpy(static_cast<unsigned char*>(buffers.at(b.alias).cpu)+b.off,bytes.data(),bytes.size());}
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex=d.family; VkCommandPool pool{}; VK(vkCreateCommandPool(d.device,&pc,nullptr,&pool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VkCommandBuffer cb{}; VK(vkAllocateCommandBuffers(d.device,&ca,&cb)); VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK(vkBeginCommandBuffer(cb,&begin));
    for(auto &i:images) {
        require(i.set<4 && i.w && i.h && i.levels && i.levels<=32,"invalid image"); auto bytes=read(i.path); i.upload=d.buffer(bytes.size()); memcpy(i.upload.cpu,bytes.data(),bytes.size());
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VkFormat(i.format); ci.extent={i.w,i.h,1}; ci.mipLevels=i.levels; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        VK(vkCreateImage(d.device,&ci,nullptr,&i.image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(d.device,i.image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=d.memory(req.memoryTypeBits,0); VK(vkAllocateMemory(d.device,&ai,nullptr,&i.memory)); VK(vkBindImageMemory(d.device,i.image,i.memory,0));
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; ib.image=i.image; ib.subresourceRange={i.aspect,0,i.levels,0,1};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        uint64_t off=0; std::vector<VkBufferImageCopy> copies;
        for(uint32_t l=0;l<i.levels;l++){uint32_t w=std::max(1u,i.w>>l),h=std::max(1u,i.h>>l); VkBufferImageCopy cp{}; cp.bufferOffset=off; cp.imageSubresource={i.aspect,l,0,1}; cp.imageExtent={w,h,1}; copies.push_back(cp); off=(off+uint64_t(w)*h*i.bpp+3)&~3ull;}
        require(off==bytes.size(),"image payload length mismatch"); vkCmdCopyBufferToImage(cb,i.upload.b,i.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,copies.size(),copies.data());
        ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=i.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VkFormat(i.format); vi.components={VkComponentSwizzle(i.sw[0]),VkComponentSwizzle(i.sw[1]),VkComponentSwizzle(i.sw[2]),VkComponentSwizzle(i.sw[3])}; vi.subresourceRange=ib.subresourceRange;
        VK(vkCreateImageView(d.device,&vi,nullptr,&i.view)); VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=si.minFilter=VK_FILTER_NEAREST; si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; si.maxLod=float(i.levels); VK(vkCreateSampler(d.device,&si,nullptr,&i.sampler));
    }
    std::vector<VkDescriptorSetLayoutBinding> ents[4];
    for(auto &b:bindings) ents[b.set].push_back({b.binding,b.type?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    for(auto &i:images) ents[i.set].push_back({i.binding,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    VkDescriptorSetLayout layouts[4]{}; for(int i=0;i<4;i++){VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=ents[i].size();ci.pBindings=ents[i].data();VK(vkCreateDescriptorSetLayout(d.device,&ci,nullptr,&layouts[i]));}
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=4;plci.pSetLayouts=layouts; VkPipelineLayout pl{};VK(vkCreatePipelineLayout(d.device,&plci,nullptr,&pl));
    auto shader=read(argv[1]); require(shader.size()%4==0,"bad SPIR-V length"); std::vector<uint32_t> words(shader.size()/4);memcpy(words.data(),shader.data(),shader.size());
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=shader.size();smci.pCode=words.data();VkShaderModule sm{};VK(vkCreateShaderModule(d.device,&smci,nullptr,&sm));
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=sm;cp.stage.pName="main";cp.layout=pl;VkPipeline pipeline{};VK(vkCreateComputePipelines(d.device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,128},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,128}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=4;dpci.poolSizeCount=3;dpci.pPoolSizes=ps;VkDescriptorPool dp{};VK(vkCreateDescriptorPool(d.device,&dpci,nullptr,&dp));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=dp;da.descriptorSetCount=4;da.pSetLayouts=layouts;VkDescriptorSet sets[4]{};VK(vkAllocateDescriptorSets(d.device,&da,sets));
    for(auto &b:bindings){VkDescriptorBufferInfo bi{buffers.at(b.alias).b,b.off,b.len};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=sets[b.set];w.dstBinding=b.binding;w.descriptorCount=1;w.descriptorType=b.type?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&bi;vkUpdateDescriptorSets(d.device,1,&w,0,nullptr);}
    for(auto &i:images){VkDescriptorImageInfo ii{i.sampler,i.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=sets[i.set];w.dstBinding=i.binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&ii;vkUpdateDescriptorSets(d.device,1,&w,0,nullptr);}
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,4,sets,0,nullptr);vkCmdDispatch(cb,dims[0],dims[1],dims[2]);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);VK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};VK(vkCreateFence(d.device,&fci,nullptr,&fence));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;VK(vkQueueSubmit(d.queue,1,&submit,fence));VK(vkWaitForFences(d.device,1,&fence,VK_TRUE,30000000000ull));
    for(auto &b:bindings) if(b.access&2){std::ofstream out(std::string(argv[3])+"/"+std::to_string(b.set)+"-"+std::to_string(b.binding)+".bin",std::ios::binary);out.write(static_cast<char*>(buffers.at(b.alias).cpu)+b.off,b.len);require(bool(out),"output write failed");}
    vkDestroyFence(d.device,fence,nullptr);vkDestroyDescriptorPool(d.device,dp,nullptr);vkDestroyPipeline(d.device,pipeline,nullptr);vkDestroyShaderModule(d.device,sm,nullptr);vkDestroyPipelineLayout(d.device,pl,nullptr);for(auto l:layouts)vkDestroyDescriptorSetLayout(d.device,l,nullptr);vkDestroyCommandPool(d.device,pool,nullptr);
    for(auto &i:images){vkDestroySampler(d.device,i.sampler,nullptr);vkDestroyImageView(d.device,i.view,nullptr);vkDestroyImage(d.device,i.image,nullptr);vkFreeMemory(d.device,i.memory,nullptr);d.release(i.upload);}for(auto &b:buffers)d.release(b.second);
    std::cout<<"dispatch completed groups="<<dims[0]<<","<<dims[1]<<","<<dims[2]<<"\n";return 0;
} catch(const std::exception &e){std::cerr<<"reference unavailable: "<<e.what()<<"\n";return 2;}
