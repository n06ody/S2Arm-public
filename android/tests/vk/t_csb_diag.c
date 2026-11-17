// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Diagnostic-only real Vulkan test. Reuse the device/contract fixture, but observe
// and forward the actual Vulkan commands rather than trusting diagnostic text.
#define main csb_contract_main
#define nvn_plat_flag csb_base_flag
#define nvn_plat_log csb_base_log
#include "t_backend_vk.c"
#undef main
#undef nvn_plat_flag
#undef nvn_plat_log

static int csb_flag = -1;
static atomic_int csb_observe, csb_submits, csb_waits;
static unsigned csb_dispatches, csb_barriers, csb_bad_masks, csb_bad_order;
static unsigned csb_logs;
static uint64_t csb_last_count, csb_expected_seq;
static int csb_bad_log;

int nvn_plat_flag(const char *name, int dflt) {
    if (!strcmp(name, "nvk_csb")) return csb_flag < 0 ? dflt : csb_flag;
    return csb_base_flag(name, dflt);
}
void nvn_plat_log(int level, const char *cls, const char *text) {
    if (text && strstr(text, "csb barrier ")) {
        unsigned long long seq = 0, key = 0, count = 0;
        if (sscanf(text, "csb barrier seq=%llu key=%llx count=%llu encoded=1", &seq, &key, &count) != 3 ||
            seq != csb_expected_seq || key != 0x1234 || count != csb_last_count + 1) csb_bad_log++;
        csb_last_count = count; csb_logs++;
    }
    csb_base_log(level, cls, text);
}

// __real declarations take their exact function types from the staged NDK 29
// headers. Each wrapper forwards unchanged to the real Vulkan loader/VVL.
extern __typeof__(vkCmdPipelineBarrier) __real_vkCmdPipelineBarrier;
extern __typeof__(vkCmdDispatch) __real_vkCmdDispatch;
extern __typeof__(vkCmdDispatchIndirect) __real_vkCmdDispatchIndirect;
extern __typeof__(vkQueueSubmit) __real_vkQueueSubmit;
extern __typeof__(vkWaitForFences) __real_vkWaitForFences;
extern __typeof__(vkGetDeviceProcAddr) __real_vkGetDeviceProcAddr;
static PFN_vkCmdPipelineBarrier csb_real_barrier;
static PFN_vkCmdDispatch csb_real_dispatch;
static PFN_vkCmdDispatchIndirect csb_real_indirect;
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
        VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
        uint32_t ni, const VkImageMemoryBarrier *im) {
    if (atomic_load(&csb_observe) && src == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {
        csb_barriers++;
        if (csb_dispatches != csb_barriers) csb_bad_order++;
        VkAccessFlags expected = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        if (dst != VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT || flags || nm != 1 || nb || ni ||
            !m || m[0].srcAccessMask != VK_ACCESS_SHADER_WRITE_BIT || m[0].dstAccessMask != expected) csb_bad_masks++;
    }
    (csb_real_barrier ? csb_real_barrier : __real_vkCmdPipelineBarrier)(cb, src, dst, flags, nm, m, nb, b, ni, im);
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (atomic_load(&csb_observe)) csb_dispatches++;
    (csb_real_dispatch ? csb_real_dispatch : __real_vkCmdDispatch)(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off) {
    if (atomic_load(&csb_observe)) csb_dispatches++;
    (csb_real_indirect ? csb_real_indirect : __real_vkCmdDispatchIndirect)(cb, b, off);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice d, const char *name) {
    PFN_vkVoidFunction p = __real_vkGetDeviceProcAddr(d,name);
    if (!p) return p; // Never make an unsupported real entry point appear available.
    if (!strcmp(name,"vkCmdPipelineBarrier")) { csb_real_barrier=(PFN_vkCmdPipelineBarrier)p; return (PFN_vkVoidFunction)__wrap_vkCmdPipelineBarrier; }
    if (!strcmp(name,"vkCmdDispatch")) { csb_real_dispatch=(PFN_vkCmdDispatch)p; return (PFN_vkVoidFunction)__wrap_vkCmdDispatch; }
    if (!strcmp(name,"vkCmdDispatchIndirect")) { csb_real_indirect=(PFN_vkCmdDispatchIndirect)p; return (PFN_vkVoidFunction)__wrap_vkCmdDispatchIndirect; }
    return p;
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    if (atomic_load(&csb_observe)) atomic_fetch_add(&csb_submits, 1);
    return __real_vkQueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    if (atomic_load(&csb_observe)) atomic_fetch_add(&csb_waits, 1);
    return __real_vkWaitForFences(d, n, f, all, timeout);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why)) { fprintf(stderr, "device: %s\n", why); return 2; }
    printf("device: %s\n", g_dev.properties.deviceName);
    memset(&g_desc, 0, sizeof g_desc); g_desc.host_device = &g_dev; g_desc.app_label = "csb-test";
    if (!open_backend()) return 2;
    nvn_gpu_layout_ent ent = {.cls=NVN_GPU_RES_SSBO,.access=NVN_GPU_ACC_W,.set=0,.binding=0,.count=1,.stages=1u<<NVN_GPU_SH_CS};
    nvn_gpu_layout lay = {.ent=&ent,.nent=1}; nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe pipe = mk_cpipe(argv[1], "comp.comp", &lay, &sh);
    uint32_t data[64] = {0}, args[3] = {14,1,1};
    nvn_gpu_buf output = make_buf(sizeof data,NVN_GPU_MEM_SHARED,data), indirect = make_buf(sizeof args,NVN_GPU_MEM_SHARED,args);
    nvn_gpu_binding bind = {.ent=0,.cls=NVN_GPU_RES_SSBO}; bind.u.b.buf=output; bind.u.b.len=sizeof data;
    extern void nvn_gpu_diag_dispatch_tag(nvn_gpu_cmd, uint64_t, uint64_t);
    // Default off; explicit off; unselected; direct; indirect; zero groups;
    // zero groups consume tag; rejected range; csb alone (no timestamp flag).
    for (int arm=0; arm<9; arm++) {
        csb_flag = arm == 0 ? -1 : arm == 1 ? 0 : 1;
        int selected = arm != 2, want = arm == 3 || arm == 4 || arm == 8;
        atomic_store(&g_csd_test, arm != 8); atomic_store(&g_csf_test, 0);
        csb_dispatches=csb_barriers=csb_bad_masks=csb_bad_order=csb_logs=0; csb_bad_log=0;
        atomic_store(&csb_submits,0); atomic_store(&csb_waits,0);
        csb_expected_seq = 100 + (uint64_t)arm;
        nvn_gpu_cmd c=begin_cmd(); nvn_gpu_set_pipe(c,pipe); nvn_gpu_bind(c,NVN_GPU_SH_CS,&bind,1);
        atomic_store(&csb_observe,1);
        if (selected) nvn_gpu_diag_dispatch_tag(c,csb_expected_seq,0x1234);
        if (arm == 4 || arm == 7) nvn_gpu_dispatch(c,pipe,0,0,0,indirect,arm == 7 ? 4 : 0);
        else nvn_gpu_dispatch(c,pipe,arm == 5 || arm == 6 ? 0 : 8,1,1,NULL,0);
        if (arm == 6) nvn_gpu_dispatch(c,pipe,8,1,1,NULL,0); // rejected tag cannot arm this dispatch
        atomic_store(&csb_observe,0);
        CHECK(csb_barriers==(unsigned)want,"CSB actual barrier arm=%d wanted=%d got=%u",arm,want,csb_barriers);
        CHECK(!csb_bad_masks && !csb_bad_order,"CSB exact scope and post-dispatch order arm=%d",arm);
        CHECK(csb_logs==(unsigned)want && !csb_bad_log,"CSB log seq/key/cumulative count arm=%d",arm);
        CHECK(!atomic_load(&csb_submits) && !atomic_load(&csb_waits),"CSB no extra submit/fence wait arm=%d",arm);
        CHECK(csb_dispatches==(unsigned)(arm != 5 && arm != 7),"CSB real dispatch count arm=%d",arm);
        nvn_gpu_err e={0};
        if (arm == 7) { nvn_gpu_cmd_abort(c); continue; }
        CHECK(OK(nvn_gpu_cmd_commit(c,(uint64_t)arm+1,csf_test_done,NULL,&e)),"CSB commit arm=%d %s",arm,e.why);
        CHECK(OK(nvn_gpu_fence_wait((uint64_t)arm+1,5000000000ull,&e)),"CSB wait arm=%d %s",arm,e.why);
        if (arm != 5) { const uint32_t *p=(const uint32_t *)nvn_gpu_buf_cpu(output); unsigned bad=0;
            for (unsigned i=0;i<64;i++) if (!p || p[i]!=i+1) bad++;
            CHECK(!bad,"CSB unchanged compute result arm=%d",arm); }
    }
    nvn_gpu_shader_release(sh); nvn_gpu_buf_release(output,NULL,NULL); nvn_gpu_buf_release(indirect,NULL,NULL);
    nvn_gpu_err e={0}; CHECK(OK(nvn_gpu_close(5000000000ull,&e)),"CSB close");
    s2a_vk_device_destroy(&g_dev,1);
    printf("CSB %s failures=%d\n",g_fail?"FAIL":"PASS",g_fail);
    return g_fail?1:0;
}
