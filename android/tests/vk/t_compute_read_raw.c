// SPDX-License-Identifier: Apache-2.0
// 独立探针：只调用 Vulkan；不链接 replay / NVN 后端。验证检测器及 external dependency 的作用。
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"raw assertion line %d: %s\n",__LINE__,#x);exit(2);}}while(0)
#define VK(x) REQUIRE((x)==VK_SUCCESS)
static unsigned hazards,other_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user){
 (void)type;(void)user;
 if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){
  if(data->pMessageIdName&&strstr(data->pMessageIdName,"SYNC-HAZARD-READ-AFTER-WRITE"))hazards++;else other_errors++;
  fprintf(stderr,"%s: %s\n",data->pMessageIdName,data->pMessage);
 }
 return VK_FALSE;
}
static VkShaderModule shader(VkDevice device,const char *dir,const char *name){
 char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
 FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long bytes=ftell(f);REQUIRE(bytes>0&&!(bytes&3));rewind(f);
 uint32_t *code=malloc((size_t)bytes);REQUIRE(code);REQUIRE(fread(code,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
 VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
 VkShaderModule module;VK(vkCreateShaderModule(device,&ci,NULL,&module));free(code);return module;
}
int main(int argc,char **argv){
 REQUIRE(argc==3);const char *mode=argv[2];
 int env=!strcmp(mode,"env")||!strcmp(mode,"transfer-env")||!strcmp(mode,"manual-env"),sync_only=!strcmp(mode,"sync-only"),external=!strncmp(mode,"external",8),normalized=!strncmp(mode,"external-graphics",17),manual=!strcmp(mode,"manual")||!strcmp(mode,"manual-env")||!strcmp(mode,"external-graphics-manual"),transfer=!strcmp(mode,"transfer")||!strcmp(mode,"transfer-env");
 REQUIRE(env||sync_only||external||manual||transfer||!strcmp(mode,"explicit"));
 const char *layer="VK_LAYER_KHRONOS_validation";
 const char *extensions[]={VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};
 VkBool32 yes=VK_TRUE;
 VkLayerSettingEXT settings[]={
  {layer,"validate_sync",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&yes},
  {layer,"syncval_shader_accesses_heuristic",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&yes}
 };
 VkLayerSettingsCreateInfoEXT setting_ci={.sType=VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,.settingCount=sync_only?1u:2u,.pSettings=settings};
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="raw-compute-read",.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pNext=env?NULL:&setting_ci,.pApplicationInfo=&app,.enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
 VkInstance instance;VK(vkCreateInstance(&ici,NULL,&instance));
 PFN_vkCreateDebugUtilsMessengerEXT create_debug=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");
 PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");REQUIRE(create_debug&&destroy_debug);
 VkDebugUtilsMessengerCreateInfoEXT debug_ci={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,.pfnUserCallback=report};
 VkDebugUtilsMessengerEXT debug;VK(create_debug(instance,&debug_ci,NULL,&debug));
 uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,NULL));REQUIRE(count);
 VkPhysicalDevice *devices=calloc(count,sizeof *devices);REQUIRE(devices);VK(vkEnumeratePhysicalDevices(instance,&count,devices));VkPhysicalDevice physical=devices[0];free(devices);
 VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);printf("RAW device=%s\n",props.deviceName);
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);VkQueueFamilyProperties *families=calloc(count,sizeof *families);REQUIRE(families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
 uint32_t family=0;for(;family<count;family++)if((families[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))break;REQUIRE(family<count);free(families);
 float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
 VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};VkDevice device;VK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
 VkBuffer buffer;VK(vkCreateBuffer(device,&bci,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
 VkPhysicalDeviceMemoryProperties mem;vkGetPhysicalDeviceMemoryProperties(physical,&mem);uint32_t type=0;
 for(;type<mem.memoryTypeCount;type++){if((req.memoryTypeBits&(1u<<type))&&(mem.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;}REQUIRE(type<mem.memoryTypeCount);
 VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};VkDeviceMemory memory;VK(vkAllocateMemory(device,&mai,NULL,&memory));VK(vkBindBufferMemory(device,buffer,memory,0));void *mapped;VK(vkMapMemory(device,memory,0,16,0,&mapped));memset(mapped,0,16);
 VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
 VkDescriptorSetLayoutCreateInfo lci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};VkDescriptorSetLayout set_layout;VK(vkCreateDescriptorSetLayout(device,&lci,NULL,&set_layout));
 VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;VK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
 VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};VkDescriptorPool descriptor_pool;VK(vkCreateDescriptorPool(device,&dpci,NULL,&descriptor_pool));
 VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};VkDescriptorSet set;VK(vkAllocateDescriptorSets(device,&dsai,&set));
 VkDescriptorBufferInfo bi={buffer,0,16};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi};vkUpdateDescriptorSets(device,1,&write,0,NULL);
 VkShaderModule cs=shader(device,argv[1],"compute_read_args.comp"),vs=shader(device,argv[1],"fullscreen.vert");
 VkPipelineShaderStageCreateInfo cs_stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=cs,.pName="main"};
 VkComputePipelineCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage=cs_stage,.layout=layout};VkPipeline compute;VK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,NULL,&compute));
 VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};
 VkSubpassDependency dep={.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=normalized?VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
 VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=external?1u:0u,.pDependencies=external?&dep:NULL};VkRenderPass pass;VK(vkCreateRenderPass(device,&rpci,NULL,&pass));
 VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.width=64,.height=64,.layers=1};VkFramebuffer fb;VK(vkCreateFramebuffer(device,&fbci,NULL,&fb));
 VkPipelineShaderStageCreateInfo vs_stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"};
 VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
 VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.rasterizerDiscardEnable=VK_TRUE,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
 VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=1,.pStages=&vs_stage,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&raster,.layout=layout,.renderPass=pass};VkPipeline graphics;VK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gpci,NULL,&graphics));
 VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool pool;VK(vkCreateCommandPool(device,&pci,NULL,&pool));
 VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer cb;VK(vkAllocateCommandBuffers(device,&cai,&cb));VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(cb,&cbi));
 const uint32_t expected[]={3,1,0,0};
 if(transfer)vkCmdUpdateBuffer(cb,buffer,0,16,expected);else{vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,compute);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(cb,1,1,1);}
 if(manual){VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&mb,0,NULL,0,NULL);}
 VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{64,64}}};vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics);vkCmdDrawIndirect(cb,buffer,0,1,16);vkCmdEndRenderPass(cb);
 // 读回屏障在消费者之后，不会掩盖记录到 draw 的 hazard。
 VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);VK(vkEndCommandBuffer(cb));
 VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;VK(vkCreateFence(device,&fci,NULL,&fence));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};VK(vkQueueSubmit(queue,1,&submit,fence));VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));REQUIRE(!memcmp(mapped,expected,16));
 printf("RAW_COMPUTE_READ mode=%s api_sync=%d api_heuristic=%d cb=%p submits=1 waits=1 RAW=%u other_errors=%u args=3,1,0,0\n",mode,!env,!env&&!sync_only,(void*)cb,hazards,other_errors);
 vkDestroyFence(device,fence,NULL);vkDestroyCommandPool(device,pool,NULL);vkDestroyPipeline(device,graphics,NULL);vkDestroyPipeline(device,compute,NULL);vkDestroyFramebuffer(device,fb,NULL);vkDestroyRenderPass(device,pass,NULL);vkDestroyShaderModule(device,vs,NULL);vkDestroyShaderModule(device,cs,NULL);vkDestroyDescriptorPool(device,descriptor_pool,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);vkDestroyDevice(device,NULL);destroy_debug(instance,debug,NULL);vkDestroyInstance(instance,NULL);
 return other_errors?1:0;
}
