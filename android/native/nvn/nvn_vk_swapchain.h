// 新后端 present：从 ANativeWindow 建 surface/swapchain，清一帧并 present。
// 表项从 instance/device 现取；失败写 why，不猜。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <android/native_window.h>
#include "vk_device.h"
#include "nvn_vk_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif
// 已提交但没有完成/呈现证明的 present：命令缓冲与 fence 不能释放/复用，等窗口退休或设备销毁。
#define NVK_SC_RETAINED 8
// 已成功 present、但 present fence 还没 signal 的呈现退休条目。下一帧/撤销/Stop 再观察；
// 不能把“队列提交完成”当成“呈现退休”，也不能在未知退休时销毁交换链。
#define NVK_SC_PRESENT_PENDING 8
struct nvn_vk_swapchain {
    struct s2a_vk_device *d;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    // 建链时窗口的发布尺寸（未按 surface min/max 夹取）。旋转/resize 用它比较，避免把
    // maxImageExtent 的合法夹取误判成尺寸变化而每帧重建。
    uint32_t window_width, window_height;
    uint64_t touch_generation;  // WSI generation for the frame currently being presented
    // B3 按游戏输出尺寸出图（debug.s2a.buffer_geometry=1 在建链时捕获一次）：
    // window 保留给按输出尺寸重建；frame_width/height 是建链时窗口的像素尺寸（view frame），
    // buffer_geometry 把缓冲改成游戏输出尺寸后 getWidth/getHeight 只报告缓冲尺寸，
    // 不能再拿它当窗口像素用——view 尺寸由 Java 布成同比例居中，窗口像素只在这里记一次。
    ANativeWindow *window;
    int buf_geometry;
    // setBuffersGeometry 没被兑现（调用成功但缓冲尺寸/夹取后的 extent 不是游戏输出尺寸）：
    // 停止按输出尺寸重建，本交换链退回普通等比 blit；create 的 memset 清零，下条交换链重试一次。
    int geometry_failed;
    uint32_t frame_width, frame_height;
    uint32_t image_count;
    VkImage images[8];
    // acquire 只有一把：提交会 wait 它，且同步调度在下一帧 acquire 前已等到渲染 fence，
    // 可以证明上一次 wait 已执行。release 按 swapchain image 各一把：它由 present 消费，
    // 渲染 fence 不能证明 present 已消费，共用一把会在“present fence 未 signal”时重复 signal。
    VkSemaphore acquire, release[8];
    // 绘制完成证明：传给 vkQueueSubmit 的渲染 fence，signal 只代表这批命令执行完。
    VkFence fence;
    // acquire 已成功但还没有提交去消费 acquire 信号量：此时销毁信号量/交换链没有完成证明，
    // Stop/撤销只能保留。提交成功后清零。
    int acquire_pending;
    // 呈现退休证明：VkSwapchainPresentFenceInfoEXT 的 present fence，signal 才代表呈现引擎
    // 已释放该图像（仍不是 scanout/显示可见的证明）。pending 队列按 present 顺序保存。
    VkFence present_pending[NVK_SC_PRESENT_PENDING];
    int npresent_pending;
    int ready;
    // 一旦 acquire/submit/present/fence 在提交后失败，acquire/release 二值信号量的状态就不再保证，
    // 这份 swapchain 不能继续呈现下一帧：拒绝新 present，等窗口退休重建（不 ACK、不伪造完成）。
    int present_failed;
    char present_fail[160];
    struct { VkFence fence; VkFence present_fence; VkCommandBuffer cmd; } retained[NVK_SC_RETAINED];
    int nretained;
    uint64_t submitted, completed, retired, submit_failed, present_failed_count, fence_timeout, retire_timeout;
    uint64_t acquire_timeout;
};
// 生产 present 的提交/退休记账（诊断与验收）。
struct nvn_vk_swapchain_present_stats {
    uint64_t submitted;      // vkQueuePresentKHR 返回成功（present 已交给呈现引擎）
    uint64_t completed;      // 渲染 fence 已 signal（GPU 绘制完成）
    uint64_t retired;        // present fence 已 signal（呈现退休；不是 scanout/显示可见）
    uint64_t submit_failed, present_failed, fence_timeout, retire_timeout, acquire_timeout;
    int retained;
    int pending;             // 仍在途的 present fence 数
    int present_failed_flag;
};
void nvn_vk_swapchain_present_stats(struct nvn_vk_swapchain_present_stats *out);
int nvn_vk_swapchain_create(struct nvn_vk_swapchain *sc, struct s2a_vk_device *d, ANativeWindow *window, char *why, size_t why_size);
int nvn_vk_swapchain_clear_present(struct nvn_vk_swapchain *sc, float r, float g, float b, float a, char *why, size_t why_size);
// 在 swapchain 图像上画一次（全屏三角形）并 present：acquire → record → PRESENT_SRC → present → fence。
int nvn_vk_swapchain_draw_present(struct nvn_vk_swapchain *sc, struct nvn_vk_pipeline_cache *pc, VkPipeline pipe,
                                  const void *verts, size_t vert_bytes, VkBuffer ubo, VkDeviceSize ubo_size,
                                  char *why, size_t why_size);
// 把一张 SHADER_READ_ONLY 纹理缩放 blit 到 swapchain 并 present（游戏窗口纹理上屏用）。
// 返回 0 = 已 present 且本帧绘制完成；1 = 本帧绘制已完成但 acquire 在有限超时内没拿到图像
// （present 跳过、pin 已释放，下一帧重试；不能让 acquire 无界阻塞窗口撤销）；<0 = 失败。
// GPU 绘制完成由渲染 fence、呈现退休由 present fence 分别记账；present fence 交给 retire 路径。
int nvn_vk_swapchain_present_image(struct nvn_vk_swapchain *sc, VkImage src, VkFormat src_format,
                                   uint32_t src_w, uint32_t src_h, const int crop[4], char *why, size_t why_size);
// 观察已成功 present 的 present fence。0 = 全部退休；1 = 仍有在途（未失败，保留 fence）；
// -1 = fence 状态不可用（驱动错误；调用方按失败保留对象）。
int nvn_vk_swapchain_retire_present(struct nvn_vk_swapchain *sc, int64_t budget_ns);
// 0 = 全部对象已退休并销毁；-1 = 呈现退休/在途命令没有证明，交换链与 surface 保留不销毁。
int nvn_vk_swapchain_destroy(struct nvn_vk_swapchain *sc);
#ifdef __cplusplus
}
#endif
