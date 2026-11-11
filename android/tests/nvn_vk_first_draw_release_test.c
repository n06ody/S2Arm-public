// Host regression: compile the actual production translation unit with dead-section
// elimination, so these fixtures can seed its private device-owned caches.
#include "../native/nvn/nvn_vk_first_draw.c"
#include <assert.h>
static int destroyed[64], order[64], serial, writes, cache_gets, unmaps, unmap_order;
#define H(T,n) ((T)(uintptr_t)(n))
static void seen(uintptr_t h) { assert(h < 64 && h); destroyed[h]++; order[h]=++serial; }
static int clear_fv_on, texture_unpins, view_unpins;
static struct nvn_vk_tex *expected_pin;
int s2a_vk_clear_fv_enabled(void) { return clear_fv_on; }
uint64_t s2a_vk_adapter_registry_generation(void) { return 1; }
void nvn_vk_texcache_unpin(struct nvn_vk_tex *t) {
    assert(t == expected_pin && view_unpins == 1 && destroyed[21] == 1 && destroyed[22] == 1);
    texture_unpins++;
}
void nvn_vk_texcache_unpin_view(struct nvn_vk_tex *t, VkImageView v) {
    assert(t == expected_pin && v == H(VkImageView,23));
    view_unpins++;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d,VkPipelineCache h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d,VkPipelineLayout h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d,VkRenderPass h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d,VkFramebuffer h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d,VkDescriptorPool h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice d,VkQueryPool h,const VkAllocationCallbacks*a){assert(d==H(VkDevice,1));(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d,VkBuffer h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
// 批量提交 fence ring（debug.s2a.fence_ring，默认关）：release_device 只在池里有“已证明完成”的槽时
// 才销毁它们；本 fixture 不种 ring 槽，所以这个替身不应被调用（被调用就是池记账错了）。
static int fences_destroyed;
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence h,const VkAllocationCallbacks*a){(void)d;(void)h;(void)a;fences_destroyed++;}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory h,const VkAllocationCallbacks*a){(void)d;(void)a;seen((uintptr_t)h);}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory h){(void)d;assert((uintptr_t)h==16);unmaps++;unmap_order=++serial;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice d,VkPipelineCache c,size_t*n,void*p){(void)d;(void)c;cache_gets++;*n=4;if(p) memset(p,0,4);return VK_SUCCESS;}
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
int __system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }
void nvm_manifest_save_sync(void){}
void nvm_manifest_stats(uint32_t*a,uint32_t*b,uint32_t*c,uint32_t*d){*a=*b=*c=*d=0;}
int nvm_manifest_count(void){return 0;}
int nvk_driver_cache_write(const char*d,const uint8_t u[16],const void*p,size_t n,char*w,size_t z){(void)d;(void)u;(void)p;(void)n;(void)w;(void)z;writes++;return 0;}
void nvn_vk_query_stats(struct nvn_vk_query_stats *s){memset(s,0,sizeof *s);}
// 时间戳能力测试桩：队列族数=1，validBits 剧本化；查询结果剧本化（两槽一对值）。
static uint32_t g_fam_bits = 64;
static uint64_t g_ts_pair[2] = {0, 0};
static VkResult g_ts_rc = VK_SUCCESS;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice ph, uint32_t *n,
                                                                    VkQueueFamilyProperties *q) {
    (void)ph;
    if (!q) { *n = 1; return; }
    memset(q, 0, (size_t)*n * sizeof *q);
    q[0].timestampValidBits = g_fam_bits;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t f, uint32_t n,
                                                     size_t sz, void *data, VkDeviceSize st,
                                                     VkQueryResultFlags fl) {
    (void)d; (void)p; (void)f; (void)sz; (void)fl;
    if (g_ts_rc != VK_SUCCESS) return g_ts_rc;
    for (uint32_t i = 0; i < n && i < 2; i++)
        *(uint64_t *)((unsigned char *)data + i * st) = g_ts_pair[i];
    return VK_SUCCESS;
}
void nvn_vk_query_release_device(void){}
void nvn_vk_sampler_cache_destroy(struct nvn_vk_sampler_cache*c){(void)c;}
void nvn_vk_program_free(struct fd_program*p){(void)p;}
// C4b：release_device 先停管线 worker（本夹具里 worker 从未启动，如实返回 0）。
int nvn_vk_pipe_worker_stop(uint32_t b, char*w, size_t n){(void)b;(void)w;(void)n;return 0;}
static void scenario(int dir, int uuid) {
    static struct s2a_vk_device dev;
    memset(destroyed,0,sizeof destroyed); memset(order,0,sizeof order); serial=0; writes=cache_gets=unmaps=unmap_order=0;
    fences_destroyed=0;   // 本 fixture 不种 fence ring 槽：release_device 不应销毁任何 fence
    dev.device=H(VkDevice,1); g_driver_dev=dir==2 ? NULL : &dev;
    g_pipeline_cache=dir==2 ? VK_NULL_HANDLE : H(VkPipelineCache,2); g_driver_uuid_ok=uuid;
    if(dir==1) strcpy(g_driver_dir,"/fixture"); else g_driver_dir[0]=0;
    if(dir!=2) { g_pipe_cache[0].used=1; g_pipe_cache[0].pipe=H(VkPipeline,3); g_pipe_cache[0].obj=&g_prog_obj[0]; }
    g_prog_obj[0].used=1; g_prog_obj[0].device=dev.device; g_prog_obj[0].users=1;
    g_prog_obj[0].vs=H(VkShaderModule,4); g_prog_obj[0].fs=H(VkShaderModule,5);
    for(int i=0;i<4;i++) g_prog_obj[0].sets[i]=H(VkDescriptorSetLayout,6+i);
    g_prog_obj[0].playout=H(VkPipelineLayout,10);
    g_fq.npend=1; g_fq.pend[0]=H(VkPipeline,12); /* no queued commands */
    g_frame_pool[0]=H(VkDescriptorPool,13); g_frame_pool[1]=H(VkDescriptorPool,14);
    g_arena_chunk_n=1; g_arena_chunks[0].buf=H(VkBuffer,15);
    g_arena_chunks[0].mem=H(VkDeviceMemory,16); g_arena_chunks[0].map=(void*)1;
    if(dir==2) { g_rp_cache_n=1; g_rp_cache[0].pass=H(VkRenderPass,11); }
    g_snap_pool_n=1; g_snap_pool[0]=(struct fd_snap_slot){.view=H(VkImageView,19),.img=H(VkImage,20),.mem=H(VkDeviceMemory,32),.bytes=4096,.owner_frame=77,.used=1};
    g_snap_pool_bytes=4096; g_snap_retired_frame=77; g_snap_evicted=2;
    fd_memcat_add(FD_MEM_SNAP,4096); fd_devmem_register(H(VkDeviceMemory,32),4096,FD_MEM_SNAP);
    fd_memcat_add(FD_MEM_ARENA,2048); fd_devmem_register(H(VkDeviceMemory,16),2048,FD_MEM_ARENA);
    g_gpu_pool[0]=H(VkQueryPool,17); g_gpu_pool[1]=H(VkQueryPool,18);
    g_gpu_npass[0]=3; g_gpu_npass[1]=5; g_gpu_armed=1; g_gpu_arm_parity=1;
    nvn_vk_first_draw_release_device(&dev);
    assert(fences_destroyed==0);   // ring 里没有已证明完成的槽（默认关）
    assert(destroyed[17]==1 && destroyed[18]==1);
    assert(!g_gpu_pool[0] && !g_gpu_pool[1]);
    assert(!g_gpu_npass[0] && !g_gpu_npass[1]);
    assert(!g_gpu_armed && g_gpu_arm_parity==-1);
    assert(destroyed[19]==1 && destroyed[20]==1 && destroyed[32]==1);
    assert(order[19] < order[20] && order[20] < order[32]);
    assert(!g_snap_pool_n && !g_snap_pool_bytes && !g_snap_retired_frame && !g_snap_evicted);
    assert(!g_snap_pool[0].view && !g_snap_pool[0].img && !g_snap_pool[0].mem && !g_snap_pool[0].owner_frame);
    assert(atomic_load(&g_devmem_reg_n)==0);
    assert(atomic_load(&g_memcat[FD_MEM_SNAP].cur)==0 && atomic_load(&g_memcat[FD_MEM_ARENA].cur)==0);
    assert(atomic_load(&g_devmem_bytes)==0);
    assert(!nvn_vk_snap_slot_reusable(1,g_frame_id,g_snap_retired_frame));
    for(int i=dir==2?4:2;i<=10;i++) if(destroyed[i]!=1) { fprintf(stderr,"dir=%d uuid=%d handle=%d destroyed=%d, expected 1\n",dir,uuid,i,destroyed[i]); exit(1); }
    if(dir!=2) for(int i=4;i<=10;i++) assert(order[3] < order[i]);
    if(dir==2) assert(destroyed[11]==1);
    assert(destroyed[12]==1);
    for(int i=13;i<=16;i++) if(destroyed[i]!=1) { fprintf(stderr,"arena/pool handle=%d destroyed=%d, expected 1\n",i,destroyed[i]); exit(1); }
    assert(unmaps==1 && unmap_order < order[16] && order[15] < order[16]);
    for(int i=4;i<=10;i++) assert(order[12] < order[i]);
    assert(writes==(dir==1&&uuid)); assert(cache_gets==(dir==1&&uuid?2:0));
    nvn_vk_first_draw_release_device(&dev);
    for(int i=dir==2?4:2;i<=10;i++) assert(destroyed[i]==1);
    if(dir==2) assert(destroyed[11]==1);
    assert(destroyed[12]==1);
    for(int i=13;i<=16;i++) assert(destroyed[i]==1);
    assert(unmaps==1);
    assert(destroyed[19]==1 && destroyed[20]==1 && destroyed[32]==1);
    assert(destroyed[17]==1 && destroyed[18]==1);
}
// 时间戳能力门槛 + 回读掩码（fd_gpu_ts_capable / fd_gpu_mask / gpu_read_frame 白盒）。
static void gpu_ts_cap_test(void) {
    static struct s2a_vk_device dev;
    memset(&dev, 0, sizeof dev);
    dev.device = H(VkDevice,1); dev.physical = H(VkPhysicalDevice,2); dev.queue_family = 0;
    dev.properties.limits.timestampPeriod = 1000.0f;   // 1 tick = 1 µs
    dev.properties.limits.timestampComputeAndGraphics = 1;
    // validBits=64 → 能力开，掩码全开
    g_gpu_ts_cap = -1; g_fam_bits = 64;
    assert(fd_gpu_ts_capable(&dev) == 1);
    assert(fd_gpu_mask() == ~0ull);
    // validBits=32 → 能力开，掩码 0xffffffff；回卷对 {0xfffffff0,0x10} → delta 0x20=32 ticks
    g_gpu_ts_cap = -1; g_fam_bits = 32;
    assert(fd_gpu_ts_capable(&dev) == 1);
    assert(fd_gpu_mask() == 0xffffffffull);
    g_gpu_frame_pool[0] = H(VkQueryPool,42); g_gpu_frame_passes[0] = 3;
    g_ts_pair[0] = 0xfffffff0ull; g_ts_pair[1] = 0x10ull; g_ts_rc = VK_SUCCESS;
    assert(nvn_vk_first_draw_gpu_read_frame(&dev, 0, NULL) == 1);
    assert(g_gpu_last_valid && g_gpu_last_ms > 0.0319 && g_gpu_last_ms < 0.0321);   // 32 µs
    assert(g_gpu_last_passes == 3);
    // NOT_READY → 返回 0，不更新 last/seq（CPU 未等 GPU 的证明之一：查询结果未就绪就放弃）
    g_ts_rc = VK_NOT_READY;
    uint64_t seq = g_gpu_frame_seq;
    assert(nvn_vk_first_draw_gpu_read_frame(&dev, 0, NULL) == 0);
    assert(g_gpu_frame_seq == seq);
    g_gpu_frame_pool[0] = VK_NULL_HANDLE; g_gpu_last_valid = 0;
    // validBits=0 → 能力关（arm 不建池）
    g_gpu_ts_cap = -1; g_fam_bits = 0;
    assert(fd_gpu_ts_capable(&dev) == 0);
    // period=0 → 能力关
    g_gpu_ts_cap = -1; g_fam_bits = 64; dev.properties.limits.timestampPeriod = 0.f;
    assert(fd_gpu_ts_capable(&dev) == 0);
    // timestampComputeAndGraphics=0 → 能力关
    dev.properties.limits.timestampPeriod = 1000.0f;
    dev.properties.limits.timestampComputeAndGraphics = 0;
    g_gpu_ts_cap = -1;
    assert(fd_gpu_ts_capable(&dev) == 0);
    g_gpu_ts_cap = -1; g_gpu_valid_bits = 0;
}
static void clear_fv_release_test(void) {
    struct s2a_vk_device dev = {.device=H(VkDevice,1)};
    struct nvn_vk_tex texture = {0};
    expected_pin = &texture;
    clear_fv_on = 1;
    g_pin_n = 1; g_pin[0] = &texture; g_pin_view[0] = H(VkImageView,23);
    g_fq.nclear = 1; g_fq.clear_fbs[0] = H(VkFramebuffer,21); g_fq.clear_views[0] = H(VkImageView,22);
    nvn_vk_first_draw_release_device(&dev);
    assert(destroyed[21] == 1 && destroyed[22] == 1 && order[21] < order[22]);
    assert(view_unpins == 1 && texture_unpins == 1 && g_pin_n == 0 && g_fq.nclear == 0);
    nvn_vk_first_draw_release_device(&dev);
    assert(destroyed[21] == 1 && destroyed[22] == 1 && view_unpins == 1 && texture_unpins == 1);
    clear_fv_on = 0;
}
int main(int argc, char **argv){ assert(argc == 3); scenario(atoi(argv[1]), atoi(argv[2])); gpu_ts_cap_test(); clear_fv_release_test(); puts("release lifecycle passed"); }
