// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Real vk_record + frame queue + lavapipe + Khronos validation; no Android device or AHB claim.
#include "../native/nvn/nvn_vk_first_draw.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
int __system_property_get(const char*n,char*v){(void)n;*v=0;return 0;}
uint64_t s2a_vk_adapter_registry_generation(void){return 1;}
// 本探针没有纹理对象，纹理写标记不是本次路径。
void s2a_vk_adapter_mark_written(VkImage image){(void)image;}
int32_t ANativeWindow_getWidth(ANativeWindow*w){(void)w;return 0;}
int32_t ANativeWindow_getHeight(ANativeWindow*w){(void)w;return 0;}
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkDescriptorSet descriptor;
static uint32_t *mapped;
static int finished;
static int finish_compute(struct s2a_vk_device*d,void*owner,int completion){
 (void)d;assert(owner==(void*)1&&completion==0);assert(*mapped==0x13579bdfu);finished++;return 0;
}
static int build_compute(struct s2a_vk_device*d,const struct s2a_record_dispatch_state*s,
 const struct s2a_record_memory*m,struct s2a_record_dispatch_output*out,struct s2a_record_result*r,void*o){
 (void)d;(void)m;(void)r;(void)o;assert(s->fq_pool==1);out->pipeline=pipeline;out->layout=layout;
 out->sets[0]=descriptor;out->set_count=1;out->fq=1;out->owner=(void*)1;out->finish=finish_compute;return 0;
}
static p_bind_program snap;
const prog_t *nvn_prog_snap_get(uint64_t serial,uint8_t keys[6][8],uint8_t *ok,uint8_t stage[6]){
 if(serial!=1)return NULL;
 memcpy(keys,snap.keys,sizeof snap.keys);*ok=snap.kok;
 memcpy(stage,snap.stage_of,sizeof snap.stage_of);return &snap.snap;
}
static int rd(void*o,uint64_t a,void*b,size_t n){(void)o;memcpy(b,(void*)(uintptr_t)a,n);return 0;}
static int wr(void*o,uint64_t a,const void*b,size_t n){(void)o;memcpy((void*)(uintptr_t)a,b,n);return 0;}
static int packet(struct s2a_vk_device*d,unsigned op,const void*p,size_t n){
 uint8_t bytes[512]={0};size_t z=(sizeof(op_hdr)+n+7)&~(size_t)7;op_hdr h={op,(uint32_t)z};
 memcpy(bytes,&h,sizeof h);memcpy(bytes+sizeof h,p,n);struct s2a_record_result result;
 struct s2a_record_memory memory={.read=rd,.write=wr};return s2a_record_exec_one(d,bytes,z,&memory,&result);
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s line %d\n",#x,__LINE__);return 2;}}while(0)
int main(int argc,char**argv){
 CHECK(argc==2);CHECK(getenv("VK_INSTANCE_LAYERS")&&strstr(getenv("VK_INSTANCE_LAYERS"),"VK_LAYER_KHRONOS_validation"));
 uint32_t n=0;CHECK(vkEnumerateInstanceLayerProperties(&n,NULL)==VK_SUCCESS&&n);VkLayerProperties *layers=calloc(n,sizeof *layers);CHECK(layers);
 CHECK(vkEnumerateInstanceLayerProperties(&n,layers)==VK_SUCCESS);int found=0;for(uint32_t i=0;i<n;i++)if(!strcmp(layers[i].layerName,"VK_LAYER_KHRONOS_validation"))found=1;free(layers);CHECK(found);
 struct s2a_vk_device d={0};char why[256]={0};CHECK(s2a_vk_device_create(&d,why,sizeof why)==0);
 CHECK(d.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU&&strstr(d.properties.deviceName,"llvmpipe"));
 VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
 VkBuffer buffer=VK_NULL_HANDLE;CHECK(vkCreateBuffer(d.device,&bi,NULL,&buffer)==VK_SUCCESS);
 VkMemoryRequirements req;vkGetBufferMemoryRequirements(d.device,buffer,&req);
 uint32_t mt=s2a_vk_memory_type(&d,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,NULL);CHECK(mt!=UINT32_MAX);
 VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
 VkDeviceMemory memory=VK_NULL_HANDLE;CHECK(vkAllocateMemory(d.device,&ai,NULL,&memory)==VK_SUCCESS);
 CHECK(vkBindBufferMemory(d.device,buffer,memory,0)==VK_SUCCESS);CHECK(vkMapMemory(d.device,memory,0,4,0,(void**)&mapped)==VK_SUCCESS);*mapped=0;
 VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
 VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
 VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;CHECK(vkCreateDescriptorSetLayout(d.device,&li,NULL,&set_layout)==VK_SUCCESS);
 VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};
 CHECK(vkCreatePipelineLayout(d.device,&pi,NULL,&layout)==VK_SUCCESS);
 VkDescriptorPoolSize ps={.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1};
 VkDescriptorPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
 VkDescriptorPool pool=VK_NULL_HANDLE;CHECK(vkCreateDescriptorPool(d.device,&pci,NULL,&pool)==VK_SUCCESS);
 VkDescriptorSetAllocateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
 CHECK(vkAllocateDescriptorSets(d.device,&di,&descriptor)==VK_SUCCESS);
 VkDescriptorBufferInfo db={.buffer=buffer,.range=4};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=descriptor,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
 vkUpdateDescriptorSets(d.device,1,&write,0,NULL);
 FILE*f=fopen(argv[1],"rb");CHECK(f);CHECK(fseek(f,0,SEEK_END)==0);long len=ftell(f);CHECK(len>0&&len%4==0);rewind(f);
 uint32_t *spirv=malloc((size_t)len);CHECK(spirv&&fread(spirv,1,(size_t)len,f)==(size_t)len);fclose(f);
 VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)len,.pCode=spirv};
 VkShaderModule module=VK_NULL_HANDLE;CHECK(vkCreateShaderModule(d.device,&mi,NULL,&module)==VK_SUCCESS);free(spirv);
 VkPipelineShaderStageCreateInfo stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"};
 VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage=stage,.layout=layout};
 CHECK(vkCreateComputePipelines(d.device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline)==VK_SUCCESS);
 CHECK(setenv("S2A_DISPATCH_FQ","1",1)==0);g_fq.dev=&d;g_fq.dev_handle=d.device;
 s2a_record_reset_state();s2a_record_set_dispatch_builder(build_compute,NULL);
 memset(&snap,0,sizeof snap);snap.snap.serial=1;snap.kok=1;memset(snap.stage_of,255,6);snap.stage_of[0]=5;
 p_bind_program_id bind={.prog=(void*)1,.serial=1,.stages=32};CHECK(packet(&d,OP_BIND_PROGRAM,&bind,sizeof bind)==0);
 p_dispatch dispatch={.x=1,.y=1,.z=1};CHECK(packet(&d,OP_DISPATCH,&dispatch,sizeof dispatch)==0);
 uint64_t enqueued=0,pending=0;s2a_record_dispatch_fq_stats(&enqueued,NULL,&pending);CHECK(enqueued==1&&pending==1&&finished==0&&g_fq.ncmd==1);
 CHECK(nvn_vk_first_draw_flush()==0);s2a_record_dispatch_fq_stats(NULL,NULL,&pending);CHECK(pending==0&&finished==1&&*mapped==0x13579bdfu);
#ifdef S2A_FQ_VVL_NEGATIVE
 // 验证观察器确实启用：无效 size 仅在负控里创建，不交给正常路径。
 VkBufferCreateInfo invalid={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=0,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
 VkBuffer invalid_buffer=VK_NULL_HANDLE;
 if(vkCreateBuffer(d.device,&invalid,NULL,&invalid_buffer)==VK_SUCCESS && invalid_buffer)
     vkDestroyBuffer(d.device,invalid_buffer,NULL);
#endif
 CHECK(s2a_vk_device_wait_idle(&d)==VK_SUCCESS);nvn_vk_first_draw_release_device(&d);
 vkDestroyPipeline(d.device,pipeline,NULL);vkDestroyShaderModule(d.device,module,NULL);vkDestroyDescriptorPool(d.device,pool,NULL);
 vkDestroyPipelineLayout(d.device,layout,NULL);vkDestroyDescriptorSetLayout(d.device,set_layout,NULL);
 vkUnmapMemory(d.device,memory);vkDestroyBuffer(d.device,buffer,NULL);vkFreeMemory(d.device,memory,NULL);
 CHECK(s2a_vk_device_destroy(&d,1)==0);fprintf(stderr,"PASS dispatch frame queue: real shader write/read, completion owner, Khronos validation on lavapipe\n");return 0;
}
