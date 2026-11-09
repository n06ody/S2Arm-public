// C4b 宿主集成回归：把生产 nvn_vk_first_draw.c 整个 TU 收进来（同 release 夹具模式），
// 直接驱动它的管线异步缝——fd_pipe_async_get / fd_pipe_drain / pend 表 / adopt 安装。
// 链接真实 nvn_vk_pipe_worker.c + nvn_first_use.c + fake Vulkan；不建真实 device。
//
// 覆盖的独立期望：
//   1. worker 未起 → ensure 拉起；提交受理后 worker 线程建管线，adopt 装进 g_pipe_cache；
//   2. 在途超时：不可跳 → STUCK 终局 + rc=2；可跳（受控 nvk_fu_skip_safe）→ 帧预算内 rc=2；
//      项标跨调用持久（stuck 配方下一笔立即跳过，不再等）；
//   3. drain：空环零产；在途结果完成后被消费；无 pend 的残留不误装；
//   4. 开关关 → ensure 返回 0（与旧同步路径同义）；
//   5. pend 的 pin：在飞期间 pobj->users>0；worker 建管线失败如实 -1（不静默当跳过）。
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../native/nvn/nvn_vk_first_draw.c"

// ---- fake Vulkan：worker 线程创建 + 可阻塞 ----
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static int g_block_create, g_create_entered, g_create_release, g_fail_next;
static int g_create_calls, g_pipeline_destroyed, g_module_destroyed, g_queue_submits;
static pthread_t g_create_thread;

static void fake_reset(void) {
    pthread_mutex_lock(&g_fmu);
    g_block_create = g_create_entered = g_create_release = g_fail_next = 0;
    g_create_calls = g_pipeline_destroyed = g_module_destroyed = g_queue_submits = 0;
    pthread_mutex_unlock(&g_fmu);
}
static void set_blocking(int on) {
    pthread_mutex_lock(&g_fmu);
    g_block_create = on; g_create_release = 0; g_create_entered = 0;
    pthread_mutex_unlock(&g_fmu);
}
static void release_block(void) {
    pthread_mutex_lock(&g_fmu);
    g_create_release = 1; pthread_cond_broadcast(&g_fcv);
    pthread_mutex_unlock(&g_fmu);
}
static int get_int(int *p) { pthread_mutex_lock(&g_fmu); int v = *p; pthread_mutex_unlock(&g_fmu); return v; }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *i,
                                                    const VkAllocationCallbacks *a, VkShaderModule *o) {
    (void)d; (void)a; if (!i || !o || !i->pCode || i->codeSize < 4) return VK_ERROR_INITIALIZATION_FAILED;
    static int n; *o = (VkShaderModule)(uintptr_t)(0x5000 + ++n); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) {
    (void)d; (void)m; (void)a; pthread_mutex_lock(&g_fmu); g_module_destroyed++; pthread_mutex_unlock(&g_fmu);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo *infos,
                                                         const VkAllocationCallbacks *alloc, VkPipeline *out) {
    (void)device; (void)cache; (void)alloc;
    if (!infos || count != 1 || !out) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_fmu);
    g_create_calls++;
    g_create_thread = pthread_self();
    g_create_entered = 1;
    pthread_cond_broadcast(&g_fcv);
    while (g_block_create && !g_create_release) pthread_cond_wait(&g_fcv, &g_fmu);
    int fail = g_fail_next; g_fail_next = 0;
    pthread_mutex_unlock(&g_fmu);
    if (fail) return VK_ERROR_DEVICE_LOST;
    *out = (VkPipeline)(uintptr_t)(0x6000 + g_create_calls);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    (void)d; (void)p; (void)a; pthread_mutex_lock(&g_fmu); g_pipeline_destroyed++; pthread_mutex_unlock(&g_fmu);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t c, const VkSubmitInfo *s, VkFence f) {
    (void)q; (void)c; (void)s; (void)f;
    pthread_mutex_lock(&g_fmu); g_queue_submits++; pthread_mutex_unlock(&g_fmu);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a){(void)d;(void)c;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d, VkRenderPass h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d, VkFramebuffer h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice d, VkQueryPool h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d, VkFence h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d, VkDeviceMemory h){(void)d;(void)h;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice d, VkPipelineCache c, size_t *n, void *p){(void)d;(void)c;(void)p;*n=0;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice d, VkSampler h, const VkAllocationCallbacks *a){(void)d;(void)h;(void)a;}

// ---- android/host 桩 ----
int __android_log_print(int p, const char *t, const char *f, ...) { (void)p; (void)t; (void)f; return 0; }
static char g_prop_pipe_async[8];
int __system_property_get(const char *name, char *value) {
    if (value) value[0] = 0;
    if (!strcmp(name, "debug.s2a.pipe_async") && value) { strcpy(value, g_prop_pipe_async); return (int)strlen(value); }
    return 0;
}
// 可控的 C4a 判定件：draw.c 的弱引用由这里的定义解析。
static int g_may_skip = 0;
int nvk_fu_skip_safe(const struct s2a_record_draw_state *st, uint64_t fr) { (void)st; (void)fr; return g_may_skip; }
uint32_t nvk_fu_item_ms(void) { return 50; }
uint32_t nvk_fu_frame_ms(void) { return 40; }
uint32_t nvk_fu_unsafe_ms(void) { return 120; }
static struct s2a_fu_budget g_harness_budget;
struct s2a_fu_budget *nvk_fu_budget_state(void) { return &g_harness_budget; }
// manifest/驱动缓存/程序对象 桩
void nvm_manifest_save_sync(void){}
void nvm_manifest_stats(uint32_t*a,uint32_t*b,uint32_t*c,uint32_t*d){*a=*b=*c=*d=0;}
int nvm_manifest_count(void){return 0;}
int nvm_manifest_enabled(void){return 0;}
int nvm_manifest_take_next(struct nvm_entry *e){(void)e;return 0;}
void nvm_manifest_configure(const char *d,const uint8_t u[16]){(void)d;(void)u;}
void nvm_manifest_note(const struct nvk_pipe_recipe *r, const uint8_t s[32]){(void)r;(void)s;}
int nvk_driver_cache_read(const char *d, const uint8_t u[16], void **p, size_t *n, char *w, size_t z){(void)d;(void)u;(void)p;(void)n;(void)w;(void)z;return -1;}
int nvk_driver_cache_write(const char*d,const uint8_t u[16],const void*p,size_t n,char*w,size_t z){(void)d;(void)u;(void)p;(void)n;(void)w;(void)z;return 0;}
void nvn_vk_query_stats(struct nvn_vk_query_stats *s){memset(s,0,sizeof *s);}
void nvn_vk_query_release_device(void){}
void nvn_vk_sampler_cache_destroy(struct nvn_vk_sampler_cache*c){(void)c;}
void nvn_vk_program_free(struct fd_program*p){(void)p;}

// ---- flush/evict 子树的边界替身：本夹具只触达 C4b 缝，这些符号在运行时不应被调用
//     （adopt 挤缓存才会走到；夹具保证缓存有空位）。若真被调到，返回安全值并留痕。
void nvn_dirty_set_chunk_pages(uint32_t kb){(void)kb;}
int nvn_vk_format_has_stencil(VkFormat f){(void)f;return 0;}
void nvn_vk_query_batch_abandoned(void){}
int nvn_vk_query_batch_completed(uint64_t s,char*w,size_t z){(void)s;(void)w;(void)z;return 0;}
void nvn_vk_query_batch_submitted(uint64_t s){(void)s;}
void nvn_vk_query_pass_end(void){}
int nvn_vk_query_resolve(struct s2a_vk_device*d,char*w,size_t z){(void)d;(void)w;(void)z;return 0;}
void nvn_vk_texcache_unpin(struct nvn_vk_tex*t){(void)t;}
void nvn_vk_texcache_unpin_view(struct nvn_vk_tex*t,VkImageView v){(void)t;(void)v;}
int nvn_vk_ts_top_resolve(struct s2a_vk_device*d,uint64_t fid,int blocking,char*w,size_t z){(void)d;(void)fid;(void)blocking;(void)w;(void)z;return 0;}
void s2a_poolahb_complete_seq(uint64_t seq){(void)seq;}
uint64_t s2a_vk_adapter_registry_generation(void){return 0;}
uint32_t s2a_vk_memory_type(const struct s2a_vk_device*d,uint32_t bits,VkMemoryPropertyFlags req,VkMemoryPropertyFlags*act){(void)d;(void)bits;(void)req;(void)act;return UINT32_MAX;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo*i,const VkAllocationCallbacks*a,VkDeviceMemory*o){(void)d;(void)i;(void)a;(void)o;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){(void)d;(void)b;(void)m;(void)o;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage i,VkImageLayout l,VkBuffer b,uint32_t n,const VkBufferImageCopy*r){(void)c;(void)i;(void)l;(void)b;(void)n;(void)r;}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c){(void)c;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d2,VkDependencyFlags f,uint32_t n1,const VkMemoryBarrier*b1,uint32_t n2,const VkBufferMemoryBarrier*b2,uint32_t n3,const VkImageMemoryBarrier*b3){(void)c;(void)s;(void)d2;(void)f;(void)n1;(void)b1;(void)n2;(void)b2;(void)n3;(void)b3;}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t q){(void)c;(void)s;(void)p;(void)q;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d,const VkBufferCreateInfo*i,const VkAllocationCallbacks*a,VkBuffer*o){(void)d;(void)i;(void)a;(void)o;return VK_ERROR_INITIALIZATION_FAILED;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo*i,const VkAllocationCallbacks*a,VkFence*o){(void)d;(void)i;(void)a;(void)o;return VK_ERROR_INITIALIZATION_FAILED;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice d,const VkPipelineCacheCreateInfo*i,const VkAllocationCallbacks*a,VkPipelineCache*o){(void)d;(void)i;(void)a;*o=VK_NULL_HANDLE;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){(void)c;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer*c){(void)d;(void)p;(void)n;(void)c;}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements*o){(void)d;(void)b;memset(o,0,sizeof*o);}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice d,VkFence f){(void)d;(void)f;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,VkPhysicalDeviceProperties*o){(void)p;memset(o,0,sizeof*o);}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d,VkQueryPool p,uint32_t f,uint32_t n,size_t z,void*out,VkDeviceSize s,VkQueryResultFlags fl){(void)d;(void)p;(void)f;(void)n;(void)z;(void)out;(void)s;(void)fl;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize s,VkMemoryMapFlags f,void**p){(void)d;(void)m;(void)o;(void)s;(void)f;(void)p;return VK_ERROR_MEMORY_MAP_FAILED;}
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){(void)d;(void)p;(void)f;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice d,uint32_t n,const VkFence*f){(void)d;(void)n;(void)f;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d,uint32_t n,const VkFence*f,VkBool32 a,uint64_t t){(void)d;(void)n;(void)f;(void)a;(void)t;return VK_SUCCESS;}

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static struct s2a_vk_device g_dev;
#define H(T,n) ((T)(uintptr_t)(n))

static void reset_fixture(void) {
    fake_reset();
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.device = H(VkDevice, 0x11);
    g_pipeline_cache = H(VkPipelineCache, 0x22);   // 预置：fd_pipeline_cache 直接返回它，不走懒建
    memset(g_pipe_cache, 0, sizeof g_pipe_cache);
    memset(g_pipe_pend, 0, sizeof g_pipe_pend);
    g_pipe_pend_seq = 0;
    memset(g_prog_obj, 0, sizeof g_prog_obj);
    g_prog_obj[0].used = 1;
    g_prog_obj[0].device = g_dev.device;
    g_prog_obj[0].playout = H(VkPipelineLayout, 0x33);
    for (int i = 0; i < 32; i++) g_prog_obj[0].sha[i] = (uint8_t)(7 + i);
    g_may_skip = 0;
    g_pipe_clock = 0;
    for (int i = 0; i < FD_PIPE_INDEX; i++) g_pipe_index[i] = -1;
    memset(&g_harness_budget, 0, sizeof g_harness_budget);
    strcpy(g_prop_pipe_async, "1");
}

static void make_spec(struct nvk_pipe_job_spec *s, unsigned variant) {
    memset(s, 0, sizeof *s);
    s->device_generation = (uint64_t)(uintptr_t)g_dev.device;
    s->device = g_dev.device;
    s->cache = g_pipeline_cache;
    s->layout = g_prog_obj[0].playout;
    s->render_pass = H(VkRenderPass, 0x44);
    s->stage_count = 2;
    static uint32_t spirv[4] = {0x07230203u, 0, 0, 0};
    s->stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    s->stages[0].spirv = spirv; s->stages[0].bytes = sizeof spirv;
    snprintf(s->stages[0].entry, sizeof s->stages[0].entry, "main");
    s->stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    s->stages[1].spirv = spirv; s->stages[1].bytes = sizeof spirv;
    snprintf(s->stages[1].entry, sizeof s->stages[1].entry, "main");
    s->ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    s->vp.viewportCount = 1; s->vp.scissorCount = 1;
    s->rs.polygonMode = VK_POLYGON_MODE_FILL; s->rs.lineWidth = 1.0f;
    s->ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    s->blend_count = 1; s->blend[0].colorWriteMask = 0xF;
    s->cb.attachmentCount = 1;
    s->dynamic_count = 2;
    s->dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
    s->dynamic[1] = VK_DYNAMIC_STATE_SCISSOR;
    s->recipe.layout_policy = NVK_PIPE_LAYOUT_POLICY;
    s->recipe.rt_count = 1;
    s->recipe.device = (uint64_t)(uintptr_t)g_dev.device;
    snprintf(s->recipe.request_id, sizeof s->recipe.request_id, "harness-%u", variant);
    for (int i = 0; i < 32; i++) s->recipe.prog_sha[i] = (uint8_t)(7 + i);   // 与 pobj->sha 一致
    s->recipe.prim = 3 + variant;
}

int main(int argc, char **argv) {
    char why[192] = "";
    reset_fixture();
    struct nvk_pipe_job_spec spec;
    VkPipeline pipe = VK_NULL_HANDLE;
    int arc;

    // 0) 开关关：ensure 返回 0（与旧路径同义）。fd_pipe_async_on 的属性值是进程内
    //    一次性缓存，本用例必须在独立进程里跑（驱动脚本以 argv[1]=="off" 调用）。
    if (argc > 1 && !strcmp(argv[1], "off")) {
        strcpy(g_prop_pipe_async, "");
        CHECK(fd_pipe_async_ensure(&g_dev, why, sizeof why) == 0, "switch off: async path disabled");
        if (fails) { printf("pipe async draw seam (off): %d FAILURES\n", fails); return 1; }
        printf("pipe async draw seam (off): ok\n");
        return 0;
    }

    // 1) worker 未启动（ensure 负责拉起——与生产 miss 段同序）：提交受理，worker 线程
    //    建管线，fd_pipe_async_get 取回结果装进 g_pipe_cache，pobj->users 完成 pin 交接。
    CHECK(fd_pipe_async_ensure(&g_dev, why, sizeof why) == 1, "worker starts on first need");
    make_spec(&spec, 0);
    arc = fd_pipe_async_get(&g_dev, NULL, &spec, &g_prog_obj[0], fd_pipe_key(&spec.recipe),
                            NULL, &pipe, why, sizeof why);
    if (arc != 0) printf("DBG case1 arc=%d why=%s\n", arc, why);
    CHECK(arc == 0, "first submit: worker pipeline adopted");
    CHECK(pipe != VK_NULL_HANDLE, "adopted pipeline handle");
    CHECK(!pthread_equal(g_create_thread, pthread_self()), "create ran on the worker thread");
    CHECK(g_pipe_cache[0].used && g_pipe_cache[0].pipe == pipe &&
          g_pipe_cache[0].obj == &g_prog_obj[0], "pipeline installed into owner cache");
    CHECK(g_prog_obj[0].users == 1, "pin folded into the cache slot's users");
    CHECK(nvk_pipe_recipe_hit(g_pipe_cache[0].key, &g_pipe_cache[0].recipe,
                              fd_pipe_key(&spec.recipe), &spec.recipe),
          "cache entry keyed by the full recipe");
    CHECK(get_int(&g_queue_submits) == 0, "worker never touches the queue");

    // 2) 在途阻塞 + 不可跳：unsafe 上限内超时 → STUCK 终局，rc=2 跳过本笔；
    //    同配方下一笔立即跳过（项标持久），不再花时间等。
    g_frame_id++;   // 新帧：帧预算重置
    set_blocking(1);
    make_spec(&spec, 1);
    arc = fd_pipe_async_get(&g_dev, NULL, &spec, &g_prog_obj[0], fd_pipe_key(&spec.recipe),
                            NULL, &pipe, why, sizeof why);
    CHECK(arc == 2, "unskippable draw past its cap is skipped");
    int pi = fd_pipe_pend_find(&spec.recipe);
    CHECK(pi >= 0 && g_pipe_pend[pi].stuck, "stuck verdict marked on the pend entry");
    CHECK(g_prog_obj[0].users >= 1, "pend pin keeps the program object alive in flight");
    arc = fd_pipe_async_get(&g_dev, NULL, &spec, &g_prog_obj[0], fd_pipe_key(&spec.recipe),
                            NULL, &pipe, why, sizeof why);
    CHECK(arc == 2, "stuck recipe skips immediately next time");
    release_block();
    set_blocking(0);
    for (int w = 0; w < 300 && fd_pipe_pend_find(&spec.recipe) >= 0; w++) {
        fd_pipe_drain(&g_dev, NULL, NULL);
        usleep(2000);
    }
    CHECK(fd_pipe_pend_find(&spec.recipe) < 0, "late result drained and installed/released");
    {
        int hit = 0;
        for (int i = 0; i < FD_PIPE_CACHE && g_pipe_cache[i].used; i++)
            if (nvk_pipe_recipe_match(&g_pipe_cache[i].recipe, &spec.recipe)) hit = 1;
        CHECK(hit, "drained result installed into the owner cache");
    }

    // 3) 可跳绘制：安全口径开 → 帧预算内等不到 → 跳过（LATE/BUDGET 类），项标持久。
    g_frame_id++;
    set_blocking(1);
    g_may_skip = 1;
    make_spec(&spec, 2);
    arc = fd_pipe_async_get(&g_dev, NULL, &spec, &g_prog_obj[0], fd_pipe_key(&spec.recipe),
                            NULL, &pipe, why, sizeof why);
    CHECK(arc == 2, "safe draw waits only inside its frame budget, then skips");
    pi = fd_pipe_pend_find(&spec.recipe);
    CHECK(pi >= 0 && (g_pipe_pend[pi].late || g_pipe_pend[pi].stuck),
          "skippable timeout marks the pend entry");
    g_may_skip = 0;
    release_block();
    set_blocking(0);
    for (int w = 0; w < 300 && fd_pipe_pend_find(&spec.recipe) >= 0; w++) {
        fd_pipe_drain(&g_dev, NULL, NULL);
        usleep(2000);
    }
    CHECK(fd_pipe_pend_find(&spec.recipe) < 0, "drain consumed the in-flight result");

    // 4) worker 建管线失败：结果 ok=0 → 如实 -1（why 带原因），不当跳过；pend 归还。
    {
        g_frame_id++;
        pthread_mutex_lock(&g_fmu);
        g_fail_next = 1;
        pthread_mutex_unlock(&g_fmu);
        make_spec(&spec, 4);
        why[0] = 0;
        arc = fd_pipe_async_get(&g_dev, NULL, &spec, &g_prog_obj[0], fd_pipe_key(&spec.recipe),
                                NULL, &pipe, why, sizeof why);
        CHECK(arc == -1 && why[0], "worker-side create failure is a real failure, not a skip");
        CHECK(fd_pipe_pend_find(&spec.recipe) < 0, "failed recipe's pend entry is released");
    }

    // 5) 空环 drain 零产；停 worker 有界。
    CHECK(fd_pipe_drain(&g_dev, NULL, NULL) == 0, "empty ring drains nothing");
    CHECK(nvn_vk_pipe_worker_stop(3000, why, sizeof why) == 0, "worker stops bounded");

    if (fails) { printf("pipe async draw seam: %d FAILURES\n", fails); return 1; }
    printf("pipe async draw seam: ok (submit/adopt/skip classes/drain/lifecycle)\n");
    return 0;
}
