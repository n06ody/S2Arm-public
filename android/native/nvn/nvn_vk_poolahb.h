// 原生内存 owner 侧的 AHB 池后备接入：只在「内存建立点」（nvnMemoryPoolInitialize → 池注册）做迁移，
// 由资源账本（vk_resid）编排事务，绘制层只做查找。
//
// 为什么只在建立点做：那时池是新的，没有纹理/GPU 消费者、没有在途引用、没有别名，迁移不涉及
// 「回写 GPU 内容 / 重建纹理状态 / 迁移别名」这些合同还没覆盖的情形。已经使用的内存若要换底，
// 必须走完整事务（方案 §3），本模块不提供入口。
//
// 默认开（debug.s2a.pool_ahb=0 可关；设备不支持 AHB / 缺 VK_EXT_queue_family_foreign 时自动回退上传）。
// 任何一项前提不满足就放弃迁移，保持原来的上传路径；
// 迁移过程用 s2a_track_backing_begin/end（正式合同）保护，失败即回滚、不留半迁移状态。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct s2a_vk_device;

// 记录设备 + 读开关 + 查 AHB 能力；不可用时返回 -1（调用者保持上传路径）。
int s2a_poolahb_init(struct s2a_vk_device *dev);
int s2a_poolahb_enabled(void);
// 池注册时尝试把该池换成 AHB 后备页（页对齐、大小在上限内、单映射、未被占用）。
int s2a_poolahb_establish(uint64_t addr, uint64_t len);
// 绘制层查找：命中返回 0 并给出 (VkBuffer, offset)；同时按当前帧提交序号登记在途引用。
int s2a_poolahb_lookup(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);
// 同 s2a_poolahb_lookup 的命中/保话语义，额外回传该后备创建时的 usage：
// compute 直绑要先核 usage（UBO 需要 UNIFORM、SSBO 需要 STORAGE），不足按 miss 处理，不硬绑。
int s2a_poolahb_lookup_usage(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off,
                             VkBufferUsageFlags *usage);
// owner 证明：VM/thread owner 在“guest writers 已停稳”后才能置 stopped=1；没有证明时 service 拒绝迁移。
void s2a_poolahb_owner_note_writers_stopped(int stopped);
// svc_mem 的“内存 SVC owner 窗口”入口：给出停稳证明并消费一次迁移（每次窗口最多迁一个池）。
// 返回 0 = 没事 / 已迁移；非 0 = 本次窗口的 writer epoch 已被撤销（串行 epoch 升级失败），
// 调用方必须终止这次映射事务（此时改映射等于在没停稳的状态下改）。
// 没有待迁移窗口时直接返回 0，且**不碰 writer epoch**（不为不存在的活去停世界）。
// 只在 svc_mem 里经弱引用调用（宿主探针不链本模块）；在这里声明，宿主单测与调用方共用同一原型。
int s2a_android_mem_owner_window(void);
// 开始一个新提交：返回单调递增序号，lookup 的保活记在该序号下（不能用帧槽奇偶，会撞号）。
uint64_t s2a_poolahb_begin_seq(void);
// 该提交完成（fence 等到）：只退休这个序号下的引用。
void s2a_poolahb_complete_seq(uint64_t seq);
// 迁移服务：需要 owner 的安全证明；返回本次迁移的窗口数。
int s2a_poolahb_service(void);
// 帧边界：把上一帧的提交标记完成（在途引用退休），并切换到新提交序号。
void s2a_poolahb_frame_boundary(uint64_t new_seq);
// Stop / 会话重建：清账本并释放本层持有的 Vulkan 资源。**恢复不了 guest 映射**（要新进程）。
void s2a_poolahb_shutdown(void);
// guest 写过池内存：推进内容版本。
int s2a_poolahb_note_write(uint64_t addr, uint64_t len);
// 池被 Finalize：账本停止新命中，后备在完成证明后释放。
void s2a_poolahb_pool_removed(uint64_t addr, uint64_t len);
struct s2a_poolahb_stats {
    uint64_t established, refused, lookups, misses;
    uint64_t bytes;
    // miss 原因：互斥且总和等于 misses
    uint64_t miss_no_pool;            // 地址不在任何已注册池
    uint64_t miss_cold;               // 在池里、池还没迁
    uint64_t miss_pool_failed;        // 池迁移失败过
    uint64_t miss_range_unregistered; // 池已迁但这段没被账本范围覆盖
    uint64_t miss_ref_full;           // 引用登记失败
    uint64_t miss_table_full;         // 内部池统计表满
    uint64_t hit_bytes;               // 零拷贝覆盖的字节（与实际上传字节对照）
};
void s2a_poolahb_stats_get(struct s2a_poolahb_stats *out);

#ifdef __cplusplus
}
#endif
