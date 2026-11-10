#include <time.h>
#include <stdio.h>
#include <android/log.h>
#include "nvn_vk_session.h"
#include "nvn_vk_format.h"
#include "nvn_vk_texcache.h"
#include "nvn_vk_readback.h"
#include "nvn_vk_compute.h"
#include "vk_record.h"
#include "vk_clear.h"

int nvn_vk_session_create(struct nvn_vk_session *s, char *why, size_t why_size) {
    if (!s) return -1;
    if (s->ready || s->device.device || s->device.instance) {
        if (why && why_size) snprintf(why, why_size, "previous session not retired; refusing replacement");
        return -1;
    }
    memset(s, 0, sizeof *s);
    if (s2a_vk_device_create(&s->device, why, why_size)) return -1;
    // #1199：有要 compute 解码的 BC 格式而解码模块建不起来 → why 写族名、整局不进直连；设备回收。
    if (nvn_vk_texcache_init(&s->textures, &s->device, why, why_size)) {
        nvn_vk_compute_pso_cache_release(&s->device);
        (void)s2a_vk_device_destroy(&s->device, 0);
        return -1;
    }
    nvn_vk_sampler_cache_init(&s->samplers, &s->device);
    nvn_vk_pipeline_init(&s->pipelines, &s->device);
    s->ready = 1;
    return 0;
}

int nvn_vk_session_attach_window(struct nvn_vk_session *s, ANativeWindow *window, char *why, size_t why_size) {
    if (!s || !s->ready) { if (why && why_size) snprintf(why, why_size, "session not ready"); return -1; }
    // 一代窗口只能有一份交换链：先挂过就必须先证明旧交换链退休。销毁失败时保留旧对象，
    // 不允许用新 create 覆盖旧结构（否则旧 swapchain/surface/present fence 全部泄漏）。
    if (s->has_window) {
        if (nvn_vk_swapchain_destroy(&s->swapchain)) {
            if (why && why_size) snprintf(why, why_size,
                "previous swapchain has no presentation-retirement proof; refusing to attach a new window");
            return -1;
        }
        s->has_window = 0;
    }
    if (nvn_vk_swapchain_create(&s->swapchain, &s->device, window, why, why_size)) {
        // 建链失败可能已经创建了 surface/信号量：释放部分对象，别把 half-built 交换链留到下一代替换。
        (void)nvn_vk_swapchain_destroy(&s->swapchain);
        return -1;
    }
    s->has_window = 1;
    return 0;
}

int nvn_vk_session_present_clear(struct nvn_vk_session *s, float r, float g, float b, float a, char *why, size_t why_size) {
    if (!s || !s->has_window) { if (why && why_size) snprintf(why, why_size, "no game window attached"); return -1; }
    return nvn_vk_swapchain_clear_present(&s->swapchain, r, g, b, a, why, why_size);
}

int nvn_vk_session_draw_present(struct nvn_vk_session *s, const char *blob_path, char *why, size_t why_size) {
    if (!s || !s->has_window || !blob_path) { if (why && why_size) snprintf(why, why_size, "no window/blob"); return -1; }
    struct nvn_vk_vertex_layout vl; memset(&vl, 0, sizeof vl);
    vl.binding_count = 1; vl.bindings[0] = (VkVertexInputBindingDescription){0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    vl.attribute_count = 1; vl.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipeline pipe = nvn_vk_pipeline_get(&s->pipelines, blob_path, s->swapchain.format, &vl, why, why_size);
    if (!pipe) return -1;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = 2608; bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer ubo = VK_NULL_HANDLE;
    if (vkCreateBuffer(s->device.device, &bci, NULL, &ubo) != VK_SUCCESS) { if (why && why_size) snprintf(why, why_size, "ubo create"); return -1; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s->device.device, ubo, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = s2a_vk_memory_type(&s->device, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(s->device.device, &ai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(s->device.device, ubo, mem, 0) != VK_SUCCESS) { vkDestroyBuffer(s->device.device, ubo, NULL); if (why && why_size) snprintf(why, why_size, "ubo memory"); return -1; }
    void *p = NULL;
    if (vkMapMemory(s->device.device, mem, 0, 2608, 0, &p) == VK_SUCCESS) { memset(p, 0, 2608); vkUnmapMemory(s->device.device, mem); }
    // 绑一张 1x1 绿纹理到 set2/bind0（c2tex 采样；c2vspos 无该绑定时无副作用）
    VkImage timg = VK_NULL_HANDLE; VkDeviceMemory tmem = VK_NULL_HANDLE; VkImageView tview = VK_NULL_HANDLE; VkSampler tsamp = VK_NULL_HANDLE;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = (VkExtent3D){1, 1, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int tex_ok = 0;
    if (vkCreateImage(s->device.device, &ici, NULL, &timg) == VK_SUCCESS) {
        VkMemoryRequirements tmr; vkGetImageMemoryRequirements(s->device.device, timg, &tmr);
        VkMemoryAllocateInfo tai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        tai.allocationSize = tmr.size; tai.memoryTypeIndex = s2a_vk_memory_type(&s->device, tmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
        if (vkAllocateMemory(s->device.device, &tai, NULL, &tmem) == VK_SUCCESS && vkBindImageMemory(s->device.device, timg, tmem, 0) == VK_SUCCESS) {
            VkImageViewCreateInfo tvci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            tvci.image = timg; tvci.viewType = VK_IMAGE_VIEW_TYPE_2D; tvci.format = VK_FORMAT_R8G8B8A8_UNORM;
            tvci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            if (vkCreateImageView(s->device.device, &tvci, NULL, &tview) == VK_SUCCESS && vkCreateSampler(s->device.device, &sci, NULL, &tsamp) == VK_SUCCESS) {
                VkCommandBufferAllocateInfo tcai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                tcai.commandPool = s->device.command_pool; tcai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; tcai.commandBufferCount = 1;
                VkCommandBuffer tcmd = VK_NULL_HANDLE;
                if (vkAllocateCommandBuffers(s->device.device, &tcai, &tcmd) == VK_SUCCESS) {
                    VkCommandBufferBeginInfo tbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; tbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(tcmd, &tbi);
                    VkImageMemoryBarrier tb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    tb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; tb.image = timg;
                    tb.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(tcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
                    VkClearColorValue green = { { 0.0f, 1.0f, 0.0f, 1.0f } };
                    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdClearColorImage(tcmd, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1, &range);
                    VkImageMemoryBarrier tb2 = tb;
                    tb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    tb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(tcmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &tb2);
                    vkEndCommandBuffer(tcmd);
                    VkSubmitInfo tsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; tsi.commandBufferCount = 1; tsi.pCommandBuffers = &tcmd;
                    VkFenceCreateInfo tfci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence tfence = VK_NULL_HANDLE; vkCreateFence(s->device.device, &tfci, NULL, &tfence);
                    if (vkQueueSubmit(s->device.queue, 1, &tsi, tfence) == VK_SUCCESS && vkWaitForFences(s->device.device, 1, &tfence, VK_TRUE, 2000000000ull) == VK_SUCCESS)
                        tex_ok = nvn_vk_pipeline_update_tex(&s->pipelines, pipe, 2, 0, tview, tsamp) == 0;
                    vkDestroyFence(s->device.device, tfence, NULL);
                    vkFreeCommandBuffers(s->device.device, s->device.command_pool, 1, &tcmd);
                }
            }
        }
    }
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    int rc = nvn_vk_swapchain_draw_present(&s->swapchain, &s->pipelines, pipe, verts, sizeof verts, ubo, 2608, why, why_size);
    if (rc == 0 && !tex_ok) { if (why && why_size) snprintf(why, why_size, "texture bind failed"); rc = -1; }
    if (tsamp) vkDestroySampler(s->device.device, tsamp, NULL);
    if (tview) vkDestroyImageView(s->device.device, tview, NULL);
    if (timg) vkDestroyImage(s->device.device, timg, NULL);
    if (tmem) vkFreeMemory(s->device.device, tmem, NULL);
    vkDestroyBuffer(s->device.device, ubo, NULL); vkFreeMemory(s->device.device, mem, NULL);
    return rc;
}

struct nvn_vk_tex *nvn_vk_session_tex(struct nvn_vk_session *s, const tex_t *t, char *why, size_t why_size) {
    if (!s || !s->ready) { if (why && why_size) snprintf(why, why_size, "session not ready"); return NULL; }
    static unsigned calls;
    if ((++calls % 50000) == 0) {
        int live, pin, gpu;
        uint64_t rn, rb, dc, dh, dby;
        nvn_vk_texcache_usage(&s->textures, &live, &pin, &gpu);
        nvn_vk_texcache_retired(&s->textures, &rn, &rb);
        nvn_vk_texcache_dirty_stats(&s->textures, &dc, &dh, &dby);
        int sidx_n = 0, sidx_cap = 0;
        nvn_vk_texcache_sidx_stats(&s->textures, &sidx_n, &sidx_cap);
        __android_log_print(ANDROID_LOG_INFO, "S2Arm",
            "texcache usage: live=%d pinned=%d gpu_written=%d sidx=%d/%d retired=%llu retired_mb=%llu dirty_checks=%llu dirty_hits=%llu reupload_mb=%llu versioned=%llu version_full=%llu gpu_cpu_conflict=%llu arm_failed=%llu partial_skip=%llu",
            live, pin, gpu, sidx_n, sidx_cap, (unsigned long long)rn, (unsigned long long)(rb >> 20),
            (unsigned long long)dc, (unsigned long long)dh, (unsigned long long)(dby >> 20),
            (unsigned long long)nvn_vk_texcache_versioned(&s->textures),
            (unsigned long long)nvn_vk_texcache_version_full(&s->textures),
            (unsigned long long)nvn_vk_texcache_gpu_cpu_conflict(&s->textures),
            (unsigned long long)nvn_vk_texcache_arm_failed(&s->textures),
            (unsigned long long)nvn_vk_texcache_partial_skipped(&s->textures));
    }
    return nvn_vk_texcache_get_tex(&s->textures, t, why, why_size);
}

int nvn_vk_session_tex_gpu_owned(struct nvn_vk_session *s, uint64_t serial) {
    if (!s || !s->ready || !serial) return 0;
    return nvn_vk_texcache_gpu_owned(&s->textures, serial);
}

void nvn_vk_session_tex_cpu_written(struct nvn_vk_session *s, const tex_t *t) {
    if (!s || !s->ready || !t) return;
    nvn_vk_texcache_mark_cpu_dirty(&s->textures, t);
}

// 内容归属：池内存被 CPU 发布了一段精确范围（nvnMemoryPoolFlushMappedRange / nvnBufferFlushMappedRange）。
void nvn_vk_session_pool_written(struct nvn_vk_session *s, const void *mem, uint64_t off, uint64_t size) {
    if (!s || !s->ready || !mem || !size) return;
    nvn_vk_texcache_mark_cpu_written_range(&s->textures, (const uint8_t *)mem + off, size);
}

// 内容归属：WriteTexels 写了一个精确矩形（level / layer 或深度切片 / 区域）。
void nvn_vk_session_tex_written_rect(struct nvn_vk_session *s, const tex_t *t, unsigned level, unsigned layer0,
                                     int x, int y, int w, int h, int z0, int nz) {
    if (!s || !s->ready || !t) return;
    nvn_vk_texcache_mark_written_rect(&s->textures, t, level, layer0, x, y, w, h, z0, nz);
}
void nvn_vk_session_tex_gpu_written_subres(struct nvn_vk_session *s, const tex_t *t, unsigned level, unsigned layer) {
    if (!s || !s->ready || !t) return;
    nvn_vk_texcache_mark_gpu_written_subres(&s->textures, t, level, layer);
}

void nvn_vk_session_tex_release(struct nvn_vk_session *s, const tex_t *t) {
    if (!s || !s->ready || !t) return;
    nvn_vk_texcache_drop_tex(&s->textures, t);
}

// 游戏窗口纹理上屏：把 tex_t 解析成会话纹理（必要时按 CPU 内容建/上传），再缩放 blit 到 swapchain。
// 纹理解析失败（格式/布局未支持）写 why 返回 -1，不拿清屏色冒充游戏画面。
// 窗口被撤销/换代：先销毁交换链（有界观察最后一帧的 present fence；拿不到呈现退休证明就保留
// swapchain/surface/present fence 并返回 -1），成功后才清 has_window。调用方只有拿到 0 才能
// 归还/ACK WSI 租约；-1 时租约必须继续持有（未知退休不释放）。
int nvn_vk_session_detach_window(struct nvn_vk_session *s) {
    if (!s || !s->has_window) return 0;
    if (nvn_vk_swapchain_destroy(&s->swapchain)) return -1;
    s->has_window = 0;
    return 0;
}

int nvn_vk_session_present_tex(struct nvn_vk_session *s, const tex_t *t, const int crop[4], char *why, size_t why_size) {
    if (!s || !s->ready || !s->has_window) { if (why && why_size) snprintf(why, why_size, "no game window attached"); return -1; }
    if (!t) { if (why && why_size) snprintf(why, why_size, "present: null texture"); return -1; }
    struct nvn_vk_tex *tex = nvn_vk_texcache_get_tex(&s->textures, t, why, why_size);
    if (!tex || !tex->image) return -1;
    // #1199：用图像实际格式（compute 解码的 BC 纹理是解码后格式），不是 nvn→vk 映射。
    VkFormat fmt = tex->vk_format;
    if (fmt == VK_FORMAT_UNDEFINED) { if (why && why_size) snprintf(why, why_size, "present: texture has no VkFormat (nvn 0x%x)", tex->nvn_format); return -1; }
    int64_t t_present0 = 0;
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) t_present0 = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    }
    // 时间线：present 开始/结束用与录制侧同一个 frame id（录制侧用 g_present_n+1）。
    uint64_t tl_fid = 0;
    {
        extern uint64_t mtl_present_count(void) __attribute__((weak));
        uint64_t done = mtl_present_count ? mtl_present_count() : 0;
        tl_fid = done + 1;
        extern void nvn_vk_frame_stats_tl_mark(uint64_t, int, double) __attribute__((weak));
        if (nvn_vk_frame_stats_tl_mark && t_present0)
            nvn_vk_frame_stats_tl_mark(tl_fid, 3 /* PRESENT_BEGIN */, (double)t_present0 / 1e9);
    }
    int rc = nvn_vk_swapchain_present_image(&s->swapchain, tex->image, fmt, tex->width, tex->height, crop, why, why_size);
    {
        struct timespec ts;
        if (t_present0 && clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            int64_t t1 = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
            extern void nvn_vk_frame_stats_tl_mark(uint64_t, int, double) __attribute__((weak));
            extern void nvn_vk_frame_stats_tl_finish(uint64_t) __attribute__((weak));
            if (nvn_vk_frame_stats_tl_mark)
                nvn_vk_frame_stats_tl_mark(tl_fid, 4 /* PRESENT_END */, (double)t1 / 1e9);
            if (nvn_vk_frame_stats_tl_finish) nvn_vk_frame_stats_tl_finish(tl_fid);
            extern void nvn_vk_frame_stats_present_span(double, double) __attribute__((weak));
            if (nvn_vk_frame_stats_present_span)
                nvn_vk_frame_stats_present_span((double)t_present0 / 1e9, (double)t1 / 1e9);
        }
    }
    {
        static unsigned present_count, skipped_count;
        if (rc == 0 && (++present_count % 60) == 0)
            __android_log_print(ANDROID_LOG_INFO, "S2Arm", "mtl_present: #%u presented %ux%u", present_count, tex->width, tex->height);
        else if (rc == 1 && (++skipped_count % 60) == 1)
            __android_log_print(ANDROID_LOG_INFO, "S2Arm",
                                "mtl_present: swapchain image unavailable (acquire timeout, %u skipped); frame completed",
                                skipped_count);
    }
    return rc;
}

int nvn_vk_session_destroy_with_callback(struct nvn_vk_session *s, void (*release)(struct s2a_vk_device *)) {
    if (!s || !s->ready) return 0;
    // 先观察账本，后拿同一次设备空闲证明。空账本也不代表没有其它已提交工作。
    uint32_t left_record = 0, left_clear = 0, left_tex = 0;
    s2a_record_release_retained(&s->device, 2000000000ull, &left_record);
    s2a_vk_clear_release_retained(&s->device, 2000000000ull, &left_clear);
    nvn_vk_texcache_release_retained(&s->device, 2000000000ull, &left_tex);
    int idle = s2a_vk_device_wait_idle(&s->device);
    if (idle != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "session destroy: device idle unproven (%d); all children/device kept", idle);
        return -1; // 纯 A：DEVICE_LOST 也保留；不放弃 CPU 账本、不声称完成。
    }
    // WaitIdle 成功不等于 CPU 账本已结清；复用各 owner 的正常回收路径重新结账。
    int r_record = s2a_record_release_retained(&s->device, 2000000000ull, &left_record);
    int r_clear = s2a_vk_clear_release_retained(&s->device, 2000000000ull, &left_clear);
    int r_tex = nvn_vk_texcache_release_retained(&s->device, 2000000000ull, &left_tex);
    if (r_record < 0 || r_clear < 0 || r_tex < 0 || left_record || left_clear || left_tex) {
        __android_log_print(ANDROID_LOG_ERROR, "S2Arm",
                            "session destroy: settlement incomplete record=%d(%u) clear=%d(%u) upload=%d(%u); "
                            "all children/device kept", r_record, left_record, r_clear, left_clear, r_tex, left_tex);
        return -1;
    }
    if (s->has_window) {
        // GPU 空闲不是呈现引擎退休证明；仍须原交换链合同，失败不拆其它孩子。
        if (nvn_vk_swapchain_destroy(&s->swapchain)) return -1;
        s->has_window = 0;
    }
    if (release) release(&s->device);
    nvn_vk_pipeline_destroy(&s->pipelines);
    nvn_vk_texcache_destroy(&s->textures);
    nvn_vk_sampler_cache_destroy(&s->samplers);
    // 读回 staging 缓存也在设备收尾里释放：等不到完成证明的缓冲由它自己保留并记日志。
    nvn_vk_session_read_texels_release(s);
    // compute PSO 进程内缓存同点释放：对象属于本设备，先于 vkDestroyDevice。
    nvn_vk_compute_pso_cache_release(&s->device);
    // 此间不再提交工作；消费上面的同一次 VK_SUCCESS，不再 WaitIdle。
    if (s2a_vk_device_destroy(&s->device, 1)) return -1;
    s->ready = 0;
    return 0;
}

// GPU 写过的纹理：CPU 镜像不再可信，缓存淘汰时不能当成可重建内容。
void nvn_vk_session_mark_written(struct nvn_vk_session *s, VkImage image) {
    if (s && s->ready) nvn_vk_texcache_mark_written(&s->textures, image);
}

int nvn_vk_session_destroy(struct nvn_vk_session *s) {
    return nvn_vk_session_destroy_with_callback(s, NULL);
}
