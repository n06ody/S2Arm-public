// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 宿主编译缝：生产 compute.c 的 bad: 事实行直接调 __android_log_print(ANDROID_LOG_INFO,...)，
// main 7afe4bfa1 用的是 weak extern + 数字级别，本分支没有为宿主编译留声明。这里在包含生产 TU
// 之前补宏/原型，并在下面给 strong 捕获替身（Android 构建仍走真实 <android/log.h>）。
#define ANDROID_LOG_INFO 4
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#include "nvn/nvn_vk_compute.c"
#include <assert.h>
#include <stdarg.h>
static int live, step, fail_at, reads, writes, write_fail, read_fail, unknown_type, low_limit;
static int pipe_calls, plc_calls;
static unsigned handle=1;
static unsigned descriptors;
static struct nvk_compute_lookup_result accepted;
static char why[256];
static VkDeviceSize sizes[4096];
static struct nvn_vk_compute_program fixture;
static uint8_t c1_bytes[0x100];
static struct s2a_source_entry_view src;
// guest 数组比“注册池”大：变异后可能把 1 MiB 上限窗口读进数组，宿主 fixture 不该先越界，
// 让失败的断言落在用例自己的 size/guest 期望上。池边界单独用 TEST_POOL_END 表示。
#define TEST_POOL_END 0x180000u
static uint8_t guest[0x210000];
static int next(void) {return ++step==fail_at;}
#define HANDLE(type) (type)(uintptr_t)(++handle)
#define CREATE(type,out) do { if(next())return VK_ERROR_OUT_OF_HOST_MEMORY; *out=HANDLE(type);live++;return VK_SUCCESS;}while(0)
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,VkPhysicalDeviceProperties*out){(void)p;memset(out,0,sizeof *out);out->limits.minUniformBufferOffsetAlignment=16;out->limits.minStorageBufferOffsetAlignment=16;out->limits.maxBoundDescriptorSets=low_limit?2:4;out->limits.maxPerStageDescriptorUniformBuffers=16;out->limits.maxDescriptorSetUniformBuffers=16;out->limits.maxPerStageDescriptorStorageBuffers=16;out->limits.maxDescriptorSetStorageBuffers=16;out->limits.maxPerStageResources=32;out->limits.maxPerStageDescriptorSampledImages=16;out->limits.maxDescriptorSetSampledImages=16;out->limits.maxUniformBufferRange=65536;out->limits.maxStorageBufferRange=1<<20;for(int i=0;i<3;i++)out->limits.maxComputeWorkGroupCount[i]=65535;}
uint32_t s2a_vk_memory_type(const struct s2a_vk_device*d,uint32_t bits,VkMemoryPropertyFlags req,VkMemoryPropertyFlags*actual){(void)d;(void)bits;(void)actual;assert(req==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));return unknown_type?UINT32_MAX:0;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d,const VkBufferCreateInfo*c,const VkAllocationCallbacks*a,VkBuffer*out){(void)d;(void)a;if(next())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=HANDLE(VkBuffer);sizes[handle]=c->size;live++;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks*a){(void)d;(void)b;(void)a;live--;}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements*r){(void)d;r->size=sizes[(uintptr_t)b];r->alignment=16;r->memoryTypeBits=1;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo*c,const VkAllocationCallbacks*a,VkDeviceMemory*out){(void)d;(void)a;if(next())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(VkDeviceMemory)malloc((size_t)c->allocationSize);assert(*out);memset((void*)*out,0xa5,(size_t)c->allocationSize);live++;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks*a){(void)d;(void)a;free((void*)m);live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){(void)d;(void)b;(void)m;assert(o==0);return next()?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize s,VkMemoryMapFlags f,void**out){(void)d;(void)o;(void)s;(void)f;if(next())return VK_ERROR_MEMORY_MAP_FAILED;*out=(void*)m;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory m){(void)d;(void)m;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo*c,const VkAllocationCallbacks*a,VkShaderModule*out){(void)d;(void)a;assert(c->codeSize==20);CREATE(VkShaderModule,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule m,const VkAllocationCallbacks*a){(void)d;(void)m;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo*c,const VkAllocationCallbacks*a,VkDescriptorSetLayout*out){(void)d;(void)a;for(uint32_t i=0;i<c->bindingCount;i++)assert(c->pBindings[i].stageFlags==VK_SHADER_STAGE_COMPUTE_BIT);CREATE(VkDescriptorSetLayout,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout s,const VkAllocationCallbacks*a){(void)d;(void)s;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo*c,const VkAllocationCallbacks*a,VkPipelineLayout*out){(void)d;(void)c;(void)a;CREATE(VkPipelineLayout,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks*a){(void)d;(void)p;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache cache,uint32_t n,const VkComputePipelineCreateInfo*c,const VkAllocationCallbacks*a,VkPipeline*out){(void)d;(void)a;pipe_calls++;assert(cache==VK_NULL_HANDLE||cache!=(VkPipelineCache)0);assert(n==1&&c->stage.stage==VK_SHADER_STAGE_COMPUTE_BIT&&!strcmp(c->stage.pName,"main"));CREATE(VkPipeline,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks*a){(void)d;(void)p;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice d,const VkPipelineCacheCreateInfo*c,const VkAllocationCallbacks*a,VkPipelineCache*out){(void)d;(void)c;(void)a;plc_calls++;CREATE(VkPipelineCache,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d,VkPipelineCache p,const VkAllocationCallbacks*a){(void)d;(void)p;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo*c,const VkAllocationCallbacks*a,VkDescriptorPool*out){(void)d;(void)c;(void)a;CREATE(VkDescriptorPool,out);}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d,VkDescriptorPool p,const VkAllocationCallbacks*a){(void)d;(void)p;(void)a;live--;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo*c,VkDescriptorSet*out){(void)d;if(next())return VK_ERROR_OUT_OF_POOL_MEMORY;for(uint32_t i=0;i<c->descriptorSetCount;i++)out[i]=HANDLE(VkDescriptorSet);return VK_SUCCESS;}
// 宿主替身：快照条目里的保活解除（解析/视图/采样器在 first_draw.c，宿主不建）。每次调用计数。
static int unpin_calls, tex_writes;
static VkImageView expected_view;
static struct nvn_vk_tex mock_tex;
static struct nvk_compute_tex_snapshot snapshot;
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet*w,uint32_t copies,const VkCopyDescriptorSet*c){(void)d;(void)copies;(void)c;descriptors=n;assert(n<=(uint32_t)fixture.binding_count);for(uint32_t i=0;i<n;i++){int infix=0;for(int j=0;j<fixture.binding_count;j++)if(w[i].dstBinding==(uint32_t)fixture.bindings[j].binding)infix=1;assert(infix);}assert(w[n-1].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||w[n-1].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    // 纹理描述符必须写成快照里的真实 view + sampler + GENERAL；空句柄等于“绑了占位资源”。
    for(uint32_t i=0;i<n;i++) if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){assert(w[i].pImageInfo&&w[i].pImageInfo->imageView==expected_view&&w[i].pImageInfo->sampler==(VkSampler)0x22&&w[i].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_GENERAL);tex_writes++;}
    else assert(w[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||w[i].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);}
void s2a_vk_nvn_texture_release_for_compute(struct nvn_vk_tex *t, VkImageView v){(void)t;(void)v;unpin_calls++;}
// 快照的解析入口在 first_draw.c（宿主不链），这里给空实现：本测试直接构造快照喂给 pipeline。
int s2a_vk_nvn_texture_bind_for_compute(struct s2a_vk_device *d, uint64_t handle,
    struct nvn_vk_tex **tex_out, VkImageView *view_out, VkSampler *sampler_out, char *why, size_t cap) {
    (void)d;(void)handle;(void)tex_out;(void)view_out;(void)sampler_out;snprintf(why,cap,"host fixture");return -1;
}
int s2a_vk_adapter_tex_facts(int id, struct s2a_vk_tex_facts *out, char *why, size_t cap) {
    (void)id;(void)out;snprintf(why,cap,"host fixture");return -1;
}
uint64_t s2a_vk_adapter_registry_generation(void){return 0;}
// 间接 storage 的宿主缝（main 7afe4bfa1；放在 parser stub 之前，w2_compute_integration/
// record_chain 会从本文件抽前缀，nvn_vk_guest_buffer_extent 已是生产 compute.c 的必需符号）：ga2ca 把一段假 GPU 窗口映射到 guest 数组；pool_find
// 把 guest 数组（去掉尾部一段）登记成池，使“未映射”和“不可读”两类反例互不混淆。
// -DS2A_TEST_NO_POOL_FIND 的独立构建不提供 strong stub，专门验“池查询缝缺席”的拒绝路径。
static int pool_missing;
uint64_t nvn_ga2ca(uint64_t a) {
    return a >= 0x5000000000ull && a < 0x5000000000ull + TEST_POOL_END ? a - 0x5000000000ull + 0x1000 : a;
}
#ifndef S2A_TEST_NO_POOL_FIND
int s2a_vk_adapter_pool_find(uint64_t addr, uint64_t len, uint64_t *base, uint64_t *size) {
    if (pool_missing || !len) return -1;
    if (addr >= 0x1000 && addr + len <= TEST_POOL_END) {
        if (base) *base = 0x1000;
        if (size) *size = TEST_POOL_END - 0x1000;
        return 0;
    }
    return -1;
}
#endif
// size=0（长度未声明）的 storage 走“活 nvnBuffer 精确长度”缝：默认 0（不可证明，必须拒绝），
// 混合有界/无界用例再显式打开。
static uint64_t guest_extent;
static int guest_extent_exact;
uint64_t nvn_vk_guest_buffer_extent(uint64_t addr, uint64_t cap, int *exact) {
    (void)addr; (void)cap;
    if (exact) *exact = guest_extent_exact;
    return guest_extent;
}
int nvn_vk_program_parse_compute(const uint8_t*b,size_t n,struct nvn_vk_compute_program*out,char*why,size_t cap){(void)b;(void)n;(void)why;(void)cap;*out=fixture;return 0;}
void nvn_vk_program_free_compute(struct nvn_vk_compute_program*p){memset(p,0,sizeof *p);}
static int rd(void*opaque,uint64_t addr,void*out,size_t n){(void)opaque;reads++;if(read_fail)return -1;assert(addr>=0x1000&&addr+n<=0x1000+sizeof guest);memcpy(out,guest+addr-0x1000,n);return 0;}
static int wr(void*opaque,uint64_t addr,const void*in,size_t n){(void)opaque;writes++;if(write_fail)return -1;assert(addr>=0x1000&&addr+n<=0x1000+sizeof guest);memcpy(guest+addr-0x1000,in,n);return 0;}
static uint32_t spv[5]={0x07230203,0x10000,0,1,0};
static uint32_t spv2[5]={0x07230203,0x10000,0,2,0};
static struct s2a_vk_device device={.device=(VkDevice)1,.physical=(VkPhysicalDevice)1};
static struct s2a_record_memory memory={.read=rd,.write=wr};
static struct s2a_record_dispatch_state state;
// 诊断捕获替身：Android log 在空口（宿主）只捕获不改变语义。
static char log_line[512];
static int log_calls;
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag;
    va_list ap; va_start(ap, fmt); vsnprintf(log_line, sizeof log_line, fmt, ap); va_end(ap);
    log_calls++;
    return 0;
}

static void reset(void){assert(live==0);accepted=(struct nvk_compute_lookup_result){.bytes=(uint8_t*)"x",.length=1,.profile={.storage_alignment=16}};step=0;fail_at=0;descriptors=0;reads=writes=write_fail=read_fail=unknown_type=low_limit=0;unpin_calls=tex_writes=0;expected_view=VK_NULL_HANDLE;memset(&snapshot,0,sizeof snapshot);memset(&mock_tex,0,sizeof mock_tex);pool_missing=log_calls=0;log_line[0]=0;guest_extent=0;guest_extent_exact=0;g_indirect_trunc_n=0;pipe_calls=plc_calls=0;g_cpso_gate=-1;memset(g_indirect_trunc,0,sizeof g_indirect_trunc);memset(&fixture,0,sizeof fixture);memset(&state,0,sizeof state);memset(&src,0,sizeof src);src.c1=c1_bytes;src.c1_bytes=sizeof c1_bytes;for(unsigned i=0;i<sizeof c1_bytes;i++)c1_bytes[i]=(uint8_t)(0x40+i);memset(guest,0x34,sizeof guest);fixture.stage.spirv=(uint8_t*)spv;fixture.stage.spirv_bytes=sizeof spv;fixture.binding_count=2;fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=0,.slot=3,.ubo_slot=0};fixture.bindings[1]=(struct fd_binding){.set=2,.binding=9,.kind=1,.slot=-1};fixture.storage[1]=(struct nvn_vk_compute_storage){.writable=1,.mapping=NVK_STORAGE_NVN,.index=7,.cb_slot=0,.cb_offset=0x380};state.stages=32;state.groups[0]=state.groups[1]=state.groups[2]=1;state.ubo_seen=1;state.ssbo_seen=1<<7;state.ubo[0].addr=0x1000;state.ubo[0].size=32;state.ssbo[7].addr=0x1100;state.ssbo[7].size=32;}
static int build(struct s2a_record_dispatch_output*out){return nvn_vk_compute_pipeline_build(&device,&state,&memory,&snapshot,&src,&accepted,out,why,sizeof why);}

#ifdef S2A_TEST_NO_POOL_FIND
// 池查询缝缺席：这个构建故意不提供 strong s2a_vk_adapter_pool_find（weak 符号为 NULL），
// 生产必须走 "pool lookup is unavailable" 的准确拒绝，不拿编造区间继续。
static int seam_absent_main(void) {
    (void)spv2;
    struct s2a_record_dispatch_output out;
    reset();
    fixture.storage[1].mapping = NVK_STORAGE_UBO_ADDRESS;
    fixture.storage[1].index = 1;
    fixture.storage[1].offset = 0x38;
    state.ubo_seen |= 2u;
    state.ubo[1].addr = 0x1200;
    state.ubo[1].size = 0x100;
    *(uint64_t *)(guest + 0x238) = 0x100000;
    if (s2a_vk_adapter_pool_find != NULL) { printf("FAIL: strong pool seam still linked\n"); return 1; }
    if (build(&out) == 0) { printf("FAIL: absent pool seam was accepted\n"); return 1; }
    if (!strstr(why, "pool lookup is unavailable")) { printf("FAIL: why=%s\n", why); return 1; }
    if (out.owner || live != 0) { printf("FAIL: partial owner after rejection\n"); return 1; }
    printf("compute pipeline host mock: absent pool seam rejection PASS\n");
    return 0;
}
#endif
int main(void){
#ifdef S2A_TEST_NO_POOL_FIND
    return seam_absent_main();
#else
    struct s2a_record_dispatch_output out;int cases=0;reset();assert(build(&out)==0);int max_step=step;assert(out.set_count==3&&reads==2&&writes==0);struct compute_owner*o=out.owner;assert(((uint8_t*)o->buffers[1].mapped)[0]==0x34);memset(o->buffers[1].mapped,0x78,32);int kept=live;assert(out.finish(&device,out.owner,-2)==0&&live==kept&&writes==0);assert(out.finish(&device,out.owner,0)==0&&live==0&&writes==1&&guest[256]==0x78);cases++;
reset();assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0&&live==0&&writes==0);cases++;
reset();assert(build(&out)==0);write_fail=1;assert(out.finish(&device,out.owner,0)==-1&&live==0);cases++;
for(int i=1;i<=max_step;i++){reset();fail_at=i;assert(build(&out)!=0&&live==0&&!out.owner&&!out.pipeline);cases++;}
reset();unknown_type=1;assert(build(&out)!=0&&live==0);cases++;
reset();read_fail=1;assert(build(&out)!=0&&live==0);cases++;
reset();state.ssbo_seen=0;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();fixture.storage[1].mapping=NVK_STORAGE_UNKNOWN;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();state.ssbo[7].addr=0x1010;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();state.ssbo[7].addr=UINT64_MAX-1;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();fixture.bindings[1].kind=2;fixture.bindings[1].tex_slot=-1;assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute texture handleIndex -1 is outside the combined slots: set=2 binding=9"));cases++;
// 反射到的纹理槽不在消费点快照里（该槽此刻没绑定）→ 明确失败，不绑占位纹理。
reset();fixture.bindings[1]=(struct fd_binding){.set=2,.binding=9,.kind=2,.slot=10,.tex_slot=1};assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute texture slot 1 (handleIndex 10) is not in the dispatch snapshot: set=2 binding=9"));cases++;
// handleIndex 超出组合槽范围（0..31）→ 拒绝；program_result 已校验 8..71 偶数，这里再守一道。
reset();fixture.bindings[1]=(struct fd_binding){.set=2,.binding=9,.kind=2,.slot=96,.tex_slot=44};assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute texture handleIndex 96 is outside the combined slots: set=2 binding=9"));cases++;
// 正向：快照条目 → COMBINED_IMAGE_SAMPLER（快照里的 view/sampler/GENERAL）；成功后所有权转移给 owner，
// finish(-1) 解除保活；完成未知（-2）时先不解除。
reset();fixture.bindings[1]=(struct fd_binding){.set=2,.binding=9,.kind=2,.slot=10,.tex_slot=1};
expected_view=(VkImageView)0x2001;
snapshot.items[0]=(struct nvk_compute_tex_binding){.slot=1,.handle=((uint64_t)9|((uint64_t)4<<20)),.tex_id=9,.samp_id=4,.tex=&mock_tex,.view=expected_view,.sampler=(VkSampler)0x22};
snapshot.count=1;
assert(build(&out)==0);assert(tex_writes==1&&descriptors==2&&reads==1&&out.set_count==3);assert(snapshot.count==0);
assert(out.finish(&device,out.owner,-2)==0&&unpin_calls==0&&live>0);
assert(out.finish(&device,out.owner,0)==0&&unpin_calls==1&&live==0);cases++;
// 失败路径不转移所有权：快照原样留给调用方释放（这里用不支持的 descriptor kind 触发失败）。
reset();fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=3,.slot=72};
snapshot.items[0]=(struct nvk_compute_tex_binding){.slot=1,.handle=9,.tex=&mock_tex,.view=(VkImageView)0x2001,.sampler=(VkSampler)0x22};
snapshot.count=1;
assert(build(&out)!=0&&live==0&&strstr(why,"kind=image"));
assert(snapshot.count==1&&unpin_calls==0);
nvk_compute_texture_snapshot_release(&snapshot);assert(snapshot.count==0&&unpin_calls==1);cases++;
// cbuf 1 = c1 常量段：内容取消费点快照，绑成 set 0 binding 3 的 UBO（0x100 字节，不读 guest）。
reset();fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=0,.slot=1,.ubo_slot=-2};
assert(build(&out)==0);{struct compute_owner*co=out.owner;assert(co->count==2&&co->buffers[0].size==0x100u&&co->buffers[0].guest==0&&!co->buffers[0].guest_backed);
assert(!memcmp(co->buffers[0].mapped,c1_bytes,0x100));assert(descriptors==2);assert(out.finish(&device,out.owner,-1)==0&&live==0);cases++;}
// c1 没捕获：必须报红并带 stage/cbuf/set/binding/c1 状态，不许绑零缓冲。
reset();fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=0,.slot=1,.ubo_slot=-2};src.c1=NULL;src.c1_bytes=0;
assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute c1 constant segment is not captured: stage=cs cbuf=1 set=0 binding=3 c1_bytes=0"));cases++;
// c1 大小不符（< 0x100）：同样拒绝，并把实际字节数写进原因。
reset();fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=0,.slot=1,.ubo_slot=-2};src.c1_bytes=0x50;
assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"c1_bytes=80"));cases++;
// cbuf 2 = 纹理句柄表当 UBO 用：明确未实现，不猜。
reset();fixture.bindings[0]=(struct fd_binding){.set=0,.binding=3,.kind=0,.slot=2,.ubo_slot=-1};
assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute c2 texture-handle bank used as a uniform buffer is not implemented: set=0 binding=3"));cases++;
// 高位 uniform 槽（用户槽 5 = cbuf 8）必须像用户槽 0 一样建 layout 并绑真实 guest 范围。
reset();fixture.bindings[0].slot=8;fixture.bindings[0].ubo_slot=5;
state.ubo_seen|=1u<<5;state.ubo[5].addr=0x1080;state.ubo[5].size=32;
assert(build(&out)==0);assert(descriptors==2&&reads==2);assert(out.finish(&device,out.owner,-1)==0&&live==0);cases++;
reset();fixture.bindings[0].slot=8;fixture.bindings[0].ubo_slot=5;
assert(build(&out)!=0&&live==0&&step==0&&strstr(why,"compute guest slot is unknown"));cases++;
reset();state.groups[1]=65536;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();fixture.bindings[0].is_support=1;fixture.support_bytes=0;fixture.support_read_count=1;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();state.ssbo[7].size=(1u<<20)+1;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();assert(build(&out)==0);struct s2a_vk_device wrong=device;wrong.device=(VkDevice)2;kept=live;assert(out.finish(&wrong,out.owner,0)==-1&&live==kept);assert(out.finish(&device,out.owner,17)==-1&&live==kept);assert(out.finish(&device,out.owner,-1)==0&&live==0);cases++;
reset();fixture.bindings[0].is_support=1;fixture.support_bytes=2608;fixture.support_read_count=2;fixture.support_reads[0]=(struct nvn_vk_c0_read){192,4};fixture.support_reads[1]=(struct nvn_vk_c0_read){1440,4};assert(nvn_vk_compute_pipeline_build(&device,&state,&memory,&snapshot,&src,&accepted,&out,why,sizeof why)==0);o=out.owner;uint8_t*support=o->buffers[0].mapped;assert(support[192]==0&&support[194]==0x80&&support[195]==0x3f&&support[1442]==0x80&&support[1443]==0x3f&&support[2000]==0xa5&&reads==1);assert(out.finish(&device,out.owner,-1)==0&&live==0);cases++;
reset();fixture.bindings[0].is_support=1;fixture.support_bytes=2608;assert(build(&out)==0);o=out.owner;assert(o->count==1&&reads==1&&descriptors==1&&o->buffers[0].guest==0x1100);assert(out.finish(&device,out.owner,-1)==0);cases++;
reset();fixture.binding_count=1;fixture.bindings[0].is_support=1;fixture.support_bytes=2608;assert(build(&out)==0);o=out.owner;assert(o->count==0&&out.set_count==0&&reads==0&&descriptors==0);assert(out.finish(&device,out.owner,-1)==0);cases++;
reset();low_limit=1;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();state.ssbo[7].addr=0x1101;assert(build(&out)!=0&&live==0&&step==0);cases++;
reset();state.ubo[0].addr=0x1001;assert(build(&out)!=0&&live==0&&step==0);cases++;
// UBO_PTR：描述符源是 c0 UBO 表项，绑定窗口 = 该 UBO 本身登记的 {addr,size}（上游/Metal 同口径）。
reset();fixture.storage[1].mapping=NVK_STORAGE_UBO_PTR;fixture.storage[1].index=1;state.ubo_seen|=2;state.ubo[1].addr=0x1100;state.ubo[1].size=32;state.ssbo_seen=0;assert(build(&out)==0);o=out.owner;assert(o->buffers[1].guest==0x1100&&o->buffers[1].size==32&&o->buffers[1].writable&&reads==2&&descriptors==2);memset(o->buffers[1].mapped,0x78,32);assert(out.finish(&device,out.owner,0)==0&&writes==1&&guest[256]==0x78);cases++;
reset();fixture.storage[1].mapping=NVK_STORAGE_UBO_PTR;fixture.storage[1].index=1;state.ubo_seen|=2;state.ubo[1].addr=0x1100;state.ubo[1].size=0;assert(build(&out)!=0&&step==0&&strstr(why,"no provable guest extent"));cases++;
reset();fixture.storage[1].mapping=NVK_STORAGE_UBO_PTR;fixture.storage[1].index=3;assert(build(&out)!=0&&step==0&&strstr(why,"guest slot is unknown"));cases++;
reset();fixture.storage[1].mapping=NVK_STORAGE_UBO_PTR;fixture.storage[1].index=16;assert(build(&out)!=0&&step==0&&live==0);cases++;
// UBO_ADDRESS：绑定 UBO 内 8B 裸指针；区间 = 注册池映射余量（上限 1MiB），绝不读指针后的长度字。
#define UADDR_SRC fixture.storage[1].mapping=NVK_STORAGE_UBO_ADDRESS;fixture.storage[1].index=1;fixture.storage[1].offset=0x38;state.ubo_seen|=2;state.ubo[1].addr=0x1200;state.ubo[1].size=0x100
#define UADDR_PTR(v) *(uint64_t*)(guest+0x238)=(v)
reset();UADDR_SRC;UADDR_PTR(0x100000);assert(build(&out)==0);o=out.owner;assert(o->buffers[1].guest==0x100000&&o->buffers[1].size==0x80000&&o->buffers[1].writable&&reads==3&&descriptors==2);memset(o->buffers[1].mapped,0x78,0x80000);assert(out.finish(&device,out.owner,0)==0&&writes==1&&guest[0xff000]==0x78);cases++;
reset();UADDR_SRC;UADDR_PTR(0x5000000000ull+0xff000);assert(build(&out)==0);o=out.owner;assert(o->buffers[1].guest==0x100000&&o->buffers[1].size==0x80000);assert(out.finish(&device,out.owner,-1)==0);cases++;
reset();UADDR_SRC;state.ubo_seen&=~2u;assert(build(&out)!=0&&step==0&&strstr(why,"source UBO is not bound"));cases++;
reset();UADDR_SRC;state.ubo[1].size=0x3c;assert(build(&out)!=0&&step==0&&strstr(why,"outside the bound user UBO"));cases++;
reset();UADDR_SRC;read_fail=1;assert(build(&out)!=0&&step==0&&strstr(why,"source is unreadable"));cases++;
reset();UADDR_SRC;UADDR_PTR(0x180400);assert(build(&out)!=0&&step==0&&strstr(why,"outside the registered pools"));cases++;
reset();UADDR_SRC;UADDR_PTR(0x100000);pool_missing=1;assert(build(&out)!=0&&step==0&&strstr(why,"outside the registered pools"));cases++;
reset();UADDR_SRC;UADDR_PTR(0x100008);assert(build(&out)!=0&&step==0&&strstr(why,"unaligned"));cases++;
reset();fixture.has_unaligned_storage_buffer=1;UADDR_SRC;UADDR_PTR(0x100008);assert(build(&out)==0);o=out.owner;assert(o->buffers[1].guest==0x100000&&o->buffers[1].size==0x80000);assert(out.finish(&device,out.owner,-1)==0);cases++;
// 截断不静默：余量 > 1MiB 截到上限，按 shader key 去重（设备上每 key 首次打一条 [s2a-direct-diag]
// 结构化行；宿主构建把 Android log 编掉，所以这里断言同一份计数/键账本，变异“截断静默”会变红）。
reset();UADDR_SRC;UADDR_PTR(0x1400);state.keys[0][0]=0x5a;assert(build(&out)==0&&g_indirect_trunc_n==1&&g_indirect_trunc[0].hits==1&&!memcmp(g_indirect_trunc[0].key,state.keys[0],8));o=out.owner;assert(o->buffers[1].guest==0x1400&&o->buffers[1].size==0x100000&&o->buffers[1].writable);assert(out.finish(&device,out.owner,-1)==0&&live==0);
assert(build(&out)==0&&g_indirect_trunc_n==1&&g_indirect_trunc[0].hits==2);o=out.owner;assert(out.finish(&device,out.owner,-1)==0&&live==0);
state.keys[0][0]=0x5b;assert(build(&out)==0&&g_indirect_trunc_n==2&&g_indirect_trunc[1].hits==1);o=out.owner;assert(out.finish(&device,out.owner,-1)==0&&live==0);cases++;
// 解算后的区间照常走可写重叠检查（两个有界 UBO_ADDRESS 窗口重叠）。
reset();fixture.binding_count=3;fixture.bindings[2]=(struct fd_binding){.set=3,.binding=10,.kind=1,.slot=-1};UADDR_SRC;fixture.storage[2]=(struct nvn_vk_compute_storage){.writable=1,.mapping=NVK_STORAGE_UBO_ADDRESS,.index=1,.offset=0x40};UADDR_PTR(0x100000);*(uint64_t*)(guest+0x240)=0x100800;assert(build(&out)!=0&&step==0&&strstr(why,"overlapping compute writable"));cases++;
// 有界 vs 无界（size=0）混合可写重叠：并集影子与独立影子会分叉回写，必须准确拒。
reset();fixture.binding_count=3;fixture.bindings[2]=(struct fd_binding){.set=3,.binding=10,.kind=1,.slot=-1};fixture.storage[2]=(struct nvn_vk_compute_storage){.writable=1,.mapping=NVK_STORAGE_NVN,.index=6};UADDR_SRC;UADDR_PTR(0x100000);state.ssbo_seen|=1<<6;state.ssbo[6].addr=0x100400;state.ssbo[6].size=0;guest_extent=0x20000;guest_extent_exact=1;assert(build(&out)!=0&&step==0&&strstr(why,"overlapping writable storage windows with different provenance"));cases++;
#undef UADDR_SRC
#undef UADDR_PTR
reset();fixture.has_unaligned_storage_buffer=1;assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0);cases++;
reset();accepted.profile.storage_alignment=8;assert(build(&out)!=0&&step==0&&strstr(why,"profile=8 device=16"));cases++;
reset();accepted.profile.storage_alignment=0;assert(build(&out)!=0&&step==0);cases++;
reset();fixture.has_unaligned_storage_buffer=1;state.ubo[0].addr=0x1200;state.ssbo[7].addr=0x1004;state.ssbo[7].size=8;guest[0]=7;assert(build(&out)==0);o=out.owner;assert(o->buffers[1].guest==0x1000&&o->buffers[1].size==12&&((uint8_t*)o->buffers[1].mapped)[0]==7);((uint8_t*)o->buffers[1].mapped)[0]=42;assert(writes==0);assert(out.finish(&device,out.owner,0)==0&&guest[0]==42&&writes==1);cases++;
reset();fixture.has_unaligned_storage_buffer=1;state.ubo[0].size=4;state.ssbo[7].addr=0x1004;state.ssbo[7].size=8;assert(build(&out)!=0&&step==0&&strstr(why,"[0x1000,0x1004) [0x1000,0x100c)"));cases++;
reset();fixture.has_unaligned_storage_buffer=1;fixture.storage[1].writable=0;state.ubo[0].size=4;state.ssbo[7].addr=0x1004;state.ssbo[7].size=8;assert(build(&out)==0);assert(out.finish(&device,out.owner,0)==0&&writes==0);cases++;
reset();fixture.bindings[0].slot=0;fixture.bindings[0].ubo_slot=-3;fixture.c0_read_count=3;fixture.c0_reads[0]=(struct nvn_vk_c0_read){0x200,12};fixture.c0_reads[1]=(struct nvn_vk_c0_read){0x210,12};fixture.c0_reads[2]=(struct nvn_vk_c0_read){0x380,12};state.groups[1]=7;assert(build(&out)==0);o=out.owner;uint8_t*c0=o->buffers[0].mapped;assert(c0[0x204]==7&&c0[0x211]==0x10&&c0[0x218]==32&&c0[0x381]==0x11&&c0[0x388]==32&&reads==1);assert(out.finish(&device,out.owner,-1)==0);cases++;
reset();fixture.bindings[0].slot=0;fixture.c0_read_count=1;fixture.c0_reads[0]=(struct nvn_vk_c0_read){0x220,12};assert(build(&out)!=0&&step==0&&strstr(why,"offset=0x220"));cases++;
reset();fixture.bindings[0].slot=0;fixture.c0_read_count=1;fixture.c0_reads[0]=(struct nvn_vk_c0_read){0x210,12};state.ubo[0].size=(uint64_t)UINT32_MAX+1;assert(build(&out)!=0&&step==0);cases++;

reset();fixture.has_unaligned_storage_buffer=1;fixture.bindings[0].kind=1;fixture.storage[0]=(struct nvn_vk_compute_storage){.mapping=NVK_STORAGE_NVN,.index=6};state.ssbo_seen|=1<<6;state.ssbo[6].addr=4;state.ssbo[6].size=4;state.ssbo[7].addr=8;state.ssbo[7].size=4;assert(build(&out)!=0&&step==0&&strstr(why,"[0x0,0x8) [0x0,0xc)"));cases++;
// ── compute PSO 缓存（debug.s2a.compute_pso_cache / 宿主 S2A_COMPUTE_PSO_CACHE，默认关）──
// 门关基线：同一 shader 连续 3 次 dispatch = 3 次 vkCreateComputePipelines（修前行为）。
reset();g_cpso_gate=0;
uint64_t cn0;nvn_vk_compute_cpso_totals(&cn0,NULL);
for(int i=0;i<3;i++){assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0&&live==0);}
assert(pipe_calls==3&&plc_calls==0);
uint64_t cn1;nvn_vk_compute_cpso_totals(&cn1,NULL);assert(cn1-cn0==3);cases++;
// 门开：同 shader 3 次 dispatch = 1 次创建；命中路径 finish/回写/-2 保活语义不变。
reset();g_cpso_gate=1;
assert(build(&out)==0);assert(pipe_calls==1&&plc_calls==1);VkPipeline p1=out.pipeline;
assert(out.finish(&device,out.owner,-1)==0);
assert(build(&out)==0);assert(pipe_calls==1&&out.pipeline==p1);o=out.owner;
memset(o->buffers[1].mapped,0x77,32);assert(out.finish(&device,out.owner,0)==0&&writes==1&&guest[256]==0x77);
assert(build(&out)==0);assert(pipe_calls==1);
assert(out.finish(&device,out.owner,-2)==0);   // 完成未知→owner 保活→entry refs>1 不可逐出
assert(out.finish(&device,out.owner,-1)==0);   // owner 引用归零→对象仍留表
assert(live>0);
nvn_vk_compute_pso_cache_release(&device);assert(live==0);cases++;
// 负控 A：布局签名相同、SPIR-V 不同 → 不同键另建管线；换回同内容即命中。
reset();g_cpso_gate=1;
assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0);assert(pipe_calls==1);VkPipeline q1=out.pipeline;
fixture.stage.spirv=(uint8_t*)spv2;
assert(build(&out)==0);assert(pipe_calls==2&&out.pipeline!=q1);assert(out.finish(&device,out.owner,-1)==0);
fixture.stage.spirv=(uint8_t*)spv;
assert(build(&out)==0);assert(pipe_calls==2&&out.pipeline==q1);assert(out.finish(&device,out.owner,-1)==0);
nvn_vk_compute_pso_cache_release(&device);assert(live==0);cases++;
// 负控 B：SPIR-V 相同、布局签名不同（binding 号变）→ 不同键另建管线。
reset();g_cpso_gate=1;
assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0);assert(pipe_calls==1);VkPipeline r1=out.pipeline;
fixture.bindings[0].binding=4;
assert(build(&out)==0);assert(pipe_calls==2&&out.pipeline!=r1);assert(out.finish(&device,out.owner,-1)==0);
nvn_vk_compute_pso_cache_release(&device);assert(live==0);cases++;
// 有界 LRU + 在途引用：超表逐出 refs==1 最旧项；在途 owner 的 entry 不被逐出；全量释放归零。
reset();g_cpso_gate=1;
struct s2a_record_dispatch_output held;assert(build(&held)==0&&pipe_calls==1);
for(int i=0;i<(int)CPSO_CAP+6;i++){spv[3]=(uint32_t)(0x1000+i);assert(build(&out)==0);assert(out.finish(&device,out.owner,-1)==0);assert(g_cpso_n<=CPSO_CAP);}
assert(pipe_calls==CPSO_CAP+7&&g_cpso_n==CPSO_CAP);
assert(held.finish(&device,held.owner,-1)==0);
nvn_vk_compute_pso_cache_release(&device);assert(live==0);cases++;
printf("compute pipeline host mock: %d scenarios, %d Vulkan failure points PASS\n",cases,max_step);return 0;
#endif
}
