// 直连 Vulkan 后端的纹理缓存：按 NVN 纹理身份（地址键 + serial + 格式/尺寸）复用；
// 同址新 serial（generation 变了）必须重建，不能把旧 image 当新内容用。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vk_device.h"
#include "nvn_obj.h"

#ifdef __cplusplus
extern "C" {
#endif
struct nvn_vk_bcdec;
// CPU 声明写的字节范围（相对纹理存储起点 = level0/layer0 起点），[lo, hi)。
// 这是内容归属的权威事实：只有声明范围能覆盖 GPU 内容；哈希只用于减少上传，不决定归属。
struct nvk_range { uint64_t lo, hi; };
// 同一图像上的一个精确视图（mip/层/格式/类型/通道映射）。换视图不重建图像、不重传 CPU 内容。
struct nvn_vk_tex_view {
    VkImageView view;
    uint32_t base_level, nlevels, base_layer, nlayers;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;   // 采样 view 与附件 view 的 aspect 不同（后者 DEPTH|STENCIL）
    VkComponentMapping comps;
    uint64_t last_use;
    uint32_t pins;      // 在途命令/描述符仍引用：>0 时禁止 LRU 回收（底图 pin 不算视图已保活）
    int valid;
};
#define NVK_TEX_VIEWS 8
#define NVK_TEX_VIEWS_MAX 1024
struct nvn_vk_tex {
    uint64_t key, serial, generation;
    // 同一 backing 的多个 serial 共享这张图像（别名共享）：主 serial + 引用 serial 列表。
    // 任一 serial Finalize 只摘除自己；最后一个 serial 离开才退役/回收。
    uint64_t ref_serial[8];
    int nref;
    uint64_t last_use;      // LRU：缓存满时淘汰最久未用的槽（同步等待完成后才可能淘汰）
    uint8_t gpu_written;    // GPU 写过：CPU 镜像不再可信，淘汰后不能拿 CPU 内容重建
    uint32_t nvn_format, width, height, layers, levels, depth;
    uint32_t nvn_target;    // NVNtextureTarget（8/9 = CUBEMAP/CUBEMAP_ARRAY；5/6/MS 与 10/BUFFER 未建图）
    uint32_t nvn_flags;     // NVNtextureFlags（0x10 LINEAR / 0x20 SPARSE 等）
    uint32_t nvn_stride;    // TextureBuilderSetStride（线性纹理行距）
    int32_t nvn_pkg_block_h;// 打包布局的 block height（0 = 自动算法）
    uint8_t stencil_mode;   // TextureBuilderSetDepthStencilMode(STENCIL)：采样 stencil 面而不是深度面
    uint16_t swz;          // NVN TextureBuilderSetSwizzle：r | g<<3 | b<<6 | a<<9
    uint8_t set_swz;
    VkImage image;
    VkFormat vk_format;     // 图像实际 VkFormat（compute 解码的 BC 纹理是解码后格式，非 nvn 映射）
    VkImageView view;       // 默认采样视图（整幅、基础格式）：深度只带 DEPTH；stencil 模式为 S8/STENCIL
    VkImageView view_att;   // 附件视图：深度+模板都需要的格式用它做 framebuffer 附件；其它格式同 view
    struct nvn_vk_tex_view *views;   // 附加视图缓存（mip/层/格式/类型覆盖），有界 LRU；按需增长
    int views_cap;                   // 容量（3D 深度切片需要 depth+1；全在途时可增长到上限）
    uint64_t view_clock;
    VkImageViewType view_type;
    VkDeviceMemory memory;
    // 子分配：内存来自大块（s2a_tex_block_alloc），offset 是块内偏移；mem_type 用于释放归还。
    // 每张纹理一次 vkAllocateMemory 会把驱动的分配/映射次数吃光（实测 2660 次 / 495 MB）。
    uint64_t mem_off;
    uint32_t mem_type;
    int mem_block;      // 子分配所属块号（-1 = 独立分配）
    uint64_t content_hash;     // 上传时 CPU 内容哈希（只对单级单层 2D 纹理）
    uint64_t content_check_ms; // 上次检查时间
    uint64_t content_created_ms; // 槽位建立时间：新建后 2 s 内用 16 ms 间隔复查（尽快把“建好后才被填”的内容传上来）
    uint8_t content_tracked;   // 1 = 命中时按内容哈希判断是否重传
    uint8_t content_dynamic;   // 1 = 见过变化，按 8 ms 间隔复查
    uint32_t pins;             // 本帧/在途命令仍引用；>0 时禁止淘汰和销毁
    // 初始上传没有拿到完成证明：图像/内存/视图已转入保留账本，槽不可命中、不可消费。
    uint8_t upload_incomplete;
    int valid;
    // 生命周期：Finalize 到达后 retired=1，不再可命中；等 pins / 视图 pin 归零后真正销毁。
    // owner 用于 unpin 路径回调释放（延迟销毁），不是所有权转移。
    uint8_t retired;
    struct nvn_vk_texcache *owner;
    uint64_t bytes;          // 条目持有的 GPU 内存字节（vkGetImageMemoryRequirements 的 size），回收统计用
    // 页级写跟踪（graphics/nvn/nvn_dirty）：span=该存储的范围句柄，dsize=覆盖字节，dgen=上次上传时观察到的写代数。
    // dirty_armed=1 时命中只查代数（脏才重传），不再整张哈希；未跟踪的存储仍走哈希兜底。
    const void *dspan;
    uint64_t dsize, dgen;
    // 跟踪所属映射的代际：AHB 换底后旧代际失效，缓存必须重新验证并重传（否则漏更新 → 噪点）。
    uint64_t dmap_gen;
    uint8_t dirty_armed;
    // ---- 批 A：子资源级内容版本（aspect + mip + layer）与 guest 内容账本（只做加法） ----
    // subres[((aspect * levels + mip) * layers + layer)]，取值 NVK_SUBRES_*：
    //   CPU   图像与 guest 内容一致（CPU 是事实来源，可在不等待 GPU 的情况下直接读 guest）；
    //   GPU   图像是 GPU 写的，guest 对应字节是旧的（图像是事实来源）；
    //   MIXED 图像 = GPU 内容 + 已合并的 CPU 局部写入（guest 不是整块事实来源，读回必须读图像）。
    // subres == NULL = 还没分配（按 CPU 处理）。aspects = 1（颜色）或 2（深度+模板各记一份）。
    uint8_t *subres;
    uint32_t subres_n;
    uint8_t subres_aspects;
    // 账本：guest 存储每 GOB（512 B = 64 B × 8 行）的哈希，记录“上次图像与 guest 同步”时的内容。
    // 命中且脏页代数变化时只重算脏 1 MiB 块里的 GOB，按哈希差定位 guest 真正变了的 512 B 段，
    // 从而只在图像上合并这些段（GPU 写过的其余内容不被旧 guest 字节覆盖）。
    // NULL = 不可用（未同步过 / 超过上限 / 分配失败），此时只能按子资源整块判定（见 .c 的限制说明）。
    uint64_t *gobs;
    uint32_t ngobs;
    uint8_t gobs_ready;
    // CPU 写通知（nvn_be_tex_cpu_written / FlushMappedRange）置位：下一次解析立刻按账本复查，
    // 不依赖脏页代数是否变化（自己写 guest 内存的路径不经过缺页）。
    uint8_t content_force;
    // ---- 内容归属账本（CPU 声明写范围，权威；与上面的哈希账本分工）----
    // pend[]：调用点（FlushMappedRange / WriteTexels）声明的待上传字节范围，下一次解析必须落到图像；
    // sync_r[]：当前 GPU 写代际内“图像 == guest”的字节范围（可跳过上传的范围）。
    // 不在 pend ∪ sync_r 里的字节永远不从 guest 覆盖图像；sync_r 被 mark_written（新的 GPU 写）清空。
    struct nvk_range *pend, *sync_r;
    uint32_t n_pend, cap_pend, n_sync, cap_sync;
    uint8_t pend_truncated, sync_truncated;   // 账本容量溢出：pend 溢出见 apply 的 fail-closed 规则
};
// 三态判据（纯函数，不读写缓存、不建资源；宿主可独立编译驱动）：
//   未绑定 = 记录指针为 NULL；已绑定但描述无效 = 有对象但快照不自洽；描述有效 = 可交给解析层。
enum { NVN_TEX_ID_UNBOUND = 0, NVN_TEX_ID_DESC_INVALID = 1, NVN_TEX_ID_DESC_VALID = 2 };
// 按 serial 的资源状态（活/已退休/从未创建/未知）。资源状态只有一个归属：texcache。
enum {
    NVN_TEX_SERIAL_NEVER = 0,    // 活表与墓碑都没有，且墓碑表从未淘汰过条目 → 可断言“从未创建”
    NVN_TEX_SERIAL_LIVE = 1,     // 活表按 serial 命中（身份优先；忽略描述符副本的尺寸）
    NVN_TEX_SERIAL_RETIRED = 2,  // 墓碑命中：对象已退休/已 Finalize，tomb 写退休时的描述快照
    NVN_TEX_SERIAL_UNKNOWN = 3,  // 墓碑表已满/淘汰过，缺席不再等于“从未创建” → 明确报未知
};
// 退休墓碑：serial + 退休时的描述快照 + 单调递增的退休代际（一次退休一个）。
struct nvn_tex_tombstone {
    uint64_t serial, generation;
    uint32_t width, height, depth, levels, layers, nvn_format, nvn_target;
};
#define NVK_TEX_TOMB_MAX 256
// 有界墓碑环：满后覆盖最旧条目；evicted > 0 时“查不到”不再能断言“从未创建”。
struct nvn_tex_tombstones {
    struct nvn_tex_tombstone e[NVK_TEX_TOMB_MAX];
    uint32_t n;        // 已写入条目数（<= NVK_TEX_TOMB_MAX）
    uint32_t next;     // 下一个覆盖位置（满了才推进）
    uint64_t evicted;  // 因表满被覆盖掉的条目数
    uint64_t generation;   // 单调退休代际
};
struct nvn_vk_texcache {
    struct s2a_vk_device *d;
    struct nvn_vk_bcdec *bc;   // #1199：bc_decode_mask 非 0 时建；NULL 则 BC 只走原生
    // 动态指针数组：条目地址稳定（pin 列表保存裸指针），按需增长；不再有全局固定格数和 LRU 淘汰。
    // 生命周期跟游戏创建/销毁走：Finalize 到达后 retired，用完（pins/视图 pin 归零）才销毁。
    struct nvn_vk_tex **slots;
    int nslots, cap;
    uint32_t reclaimed, evicted;
    uint64_t next_generation;
    uint64_t clock;
    uint64_t created, finalized;   // 诊断：创建/销毁计数，用来对照活纹理数与图像数
    uint64_t retired_n, retired_bytes;   // 已退役、等引用归零回收的条目数与字节（不混进 live）
    uint64_t alias_shared;               // 同一存储上出现第二个活对象的次数（别名共享尚未实现，诊断用）
    uint64_t dirty_checks, dirty_hits, reupload_bytes;   // 页级写跟踪：检查次数、脏命中次数、重传字节
    uint64_t dirty_versioned;            // 在途脏重传改成“退役旧条目 + 新建图像”的次数（旧 draw 看旧版本）
    uint64_t arm_failed;                 // 存储不在页跟踪范围内的纹理数（仍走整张哈希兜底）
    uint64_t version_full;               // 同一存储在途旧版本已达上限、本帧背压（不覆盖）的次数
    uint64_t gpu_cpu_conflict;           // GPU 写过的纹理收到 CPU 写通知、按 GPU 内容优先跳过的次数
    // serial → 活对象 的哈希提示（桶值 = slots 下标 + 1，0 = 空）。世界帧每笔纹理绑定都要按
    // serial 找活对象，此前是“遍历所有活条目 × 每个条目的 serial 列表”（实测 3400 个活纹理时
    // 占整帧 CPU 的 15%）。索引只是提示：命中后仍验证那个对象仍是活对象且仍持有该 serial，
    // 不成立就回退一次全表扫描并刷新，所以槽复用、别名增减、退役都不需要在索引里同步维护。
    uint64_t *sidx_key;
    int32_t *sidx_val;
    int sidx_cap, sidx_n;
    int sidx_busy;   // 重建期间的递归保护
    // 批 A：内容交接记账（诊断/验收；不改变行为）。gobs_* 是账本扫描与命中，patches 是局部合并次数，
    // full_covers 是整子资源覆盖（按整块上传）次数，ledger_fail 是账本不可用而只能整块判定的次数，
    // gpu_marks 是 GPU 写登记次数，mixed 是局部合并后落成 MIXED 的子资源数（累计）。
    uint64_t content_checks, gobs_scanned, gobs_changed, patches, full_covers, ledger_fail, gpu_marks, mixed;
    // 内容归属账本（声明写范围）诊断：发布/应用/跳过/未解释变化 fail-closed/容量溢出。
    uint64_t ranges_published, ranges_applied, range_skips, hint_unexplained, range_truncated;
    // 在途（pinned）且只有局部子资源被 CPU 更新：无法安全地在新图像里合并 → fail-closed 拒绝的次数。
    uint64_t mixed_versioned_rejects;
    // 当前活条目持有的 guest 内容账本字节数（每 512 B GOB 8 B；真机读数用，判断内存是否可接受）。
    uint64_t ledger_bytes;
    // A3：已退休对象的墓碑（有界环）。退休时先记身份与描述快照，再清活条目字段。
    struct nvn_tex_tombstones tombstones;
};
// 返回 0 成功；设备有需要 compute 解码的 BC 格式而解码模块建不起来时返回 -1（why 写族名，
// 上层据此整局不进直连）。why 可 NULL。
int nvn_vk_texcache_init(struct nvn_vk_texcache *c, struct s2a_vk_device *d, char *why, size_t why_size);
// 上传提交的记账（诊断/验收）：正常完成后 staging/CB/fence 逐项回收；完成未知时全部保留。
struct nvn_vk_tex_upload_stats {
    uint64_t commands_created, commands_freed;
    uint64_t fences_created, fences_freed;
    uint64_t staging_created, staging_freed;
    uint64_t submitted, completed, unsubmitted, uncompleted;
    uint64_t images_retained;     // 完成未知时转移给保留账本的图像数
    uint32_t retained_entries;    // 保留账本条目（staging/CB/fence/image/view）
    uint64_t retained_overflow;
};
void nvn_vk_texcache_upload_stats(struct nvn_vk_tex_upload_stats *out);
// 设备销毁前结算上传保留账本：有界等待 fence，拿到完成的条目显式销毁，其余保留并计数。
int nvn_vk_texcache_release_retained(struct s2a_vk_device *d, uint64_t timeout_ns, uint32_t *left_out);
// 诊断/测试注入：1 = 真实提交后按“完成未知”处理；2 = 假装提交失败；0 = 正常语义。
void nvn_vk_texcache_set_upload_fault(int mode);
// 帧内引用计数：命令缓冲/描述符仍引用该图像时禁止淘汰/销毁；flush 完成后再 unpin。
// 只查已有活纹理；调用者持有会话锁，失效或未命中返回 NULL。
struct nvn_vk_tex *nvn_vk_texcache_find_serial(struct nvn_vk_texcache *c, uint64_t serial);
void nvn_vk_texcache_pin(struct nvn_vk_tex *t);
void nvn_vk_texcache_unpin(struct nvn_vk_tex *t);
void nvn_vk_texcache_destroy(struct nvn_vk_texcache *c);
// 诊断：serial 索引条目数/容量（条目必须有界，否则“越玩越慢”）。
void nvn_vk_texcache_sidx_stats(struct nvn_vk_texcache *c, int *n, int *cap);
// 取得纹理；key=NVN 地址/serial；data 为紧凑像素（可为 NULL：只建 image 不上传）。
// 同 key+serial+格式+尺寸 → 复用；否则销毁旧 image 重建（reclaimed++）。
struct nvn_vk_tex *nvn_vk_texcache_get(struct nvn_vk_texcache *c, uint64_t key, uint64_t serial,
                                      uint32_t nvn_format, uint32_t width, uint32_t height,
                                      const void *data, size_t bytes);
// 共享前端 tex_t → 缓存。目标分类按 NVNtextureTarget：2D/2D_ARRAY/3D/RECTANGLE 与 CUBEMAP(8)/
// CUBEMAP_ARRAY(9) 建图（立方体贴图需要 CUBE_COMPATIBLE 与 CUBE/CUBE_ARRAY 视图）；多采样 5/6 与
// BUFFER(10) 没有通路，返回 NULL 并写准确原因，不按别的目标凑。
// 诊断：缓存占用（live/被 pin/GPU 写过），用于判断满载是工作集过大还是 pin 泄漏。
void nvn_vk_texcache_usage(struct nvn_vk_texcache *c, int *live, int *pinned, int *gpu_written);
// 诊断：已退役、等引用归零回收的条目数与字节（live 之外单独计）。
void nvn_vk_texcache_retired(struct nvn_vk_texcache *c, uint64_t *n, uint64_t *bytes);
// 诊断：页级写跟踪的检查/命中/重传字节（判断“干净直接复用”覆盖了多少）。
void nvn_vk_texcache_dirty_stats(struct nvn_vk_texcache *c, uint64_t *checks, uint64_t *hits, uint64_t *bytes);
// 诊断：在途脏重传走“退役旧条目 + 新建图像”的次数。
uint64_t nvn_vk_texcache_versioned(struct nvn_vk_texcache *c);
// 诊断：存储不在页跟踪范围、只能走整张哈希兜底的纹理数。
uint64_t nvn_vk_texcache_arm_failed(struct nvn_vk_texcache *c);
// 诊断：spare 额度用满、退回就地重传的次数。
uint64_t nvn_vk_texcache_version_full(struct nvn_vk_texcache *c);
uint64_t nvn_vk_texcache_gpu_cpu_conflict(struct nvn_vk_texcache *c);
// 真实 gpu_owned：按 serial 查活条目当前内容是否归 GPU（任一子资源为 GPU/MIXED 即为 1）。
int nvn_vk_texcache_gpu_owned(struct nvn_vk_texcache *c, uint64_t serial);
// ---- 批 A：子资源内容版本查询（读回/绘制选择用；只读，不改状态） ----
// aspect 0 = 颜色（或深度面），1 = 模板面（仅深度+模板格式有）。level 是 mip，layer 是数组层；
// 3D 纹理只有 layer 0（一个 Vulkan 子资源含全部 depth 切片）。
enum { NVK_SUBRES_CPU = 0, NVK_SUBRES_GPU = 1, NVK_SUBRES_MIXED = 2 };
#define NVK_SUBRES_ASPECT_COLOR 0
#define NVK_SUBRES_ASPECT_DEPTH 0
#define NVK_SUBRES_ASPECT_STENCIL 1
int nvn_vk_texcache_subres_state(const struct nvn_vk_tex *t, unsigned aspect, unsigned level, unsigned layer);
const char *nvn_vk_texcache_subres_state_name(int state);
// 这一组 (level, layer0..layer0+nlayers-1) 是否全部由 CPU 内容构成：1 = 可直接读 guest 内存；
// 0 = 至少要读图像（含 GPU 内容或混合）。没有缓存条目（从没被 GPU 用过）按 CPU 处理。
int nvn_vk_texcache_region_cpu(const struct nvn_vk_tex *t, unsigned level, unsigned layer0, unsigned nlayers);
// 按 serial 的只读版本（读回入口用：没有条目 = 从没被 GPU 用过 = 全 CPU）。不建图像、不上传。
int nvn_vk_texcache_read_guest_ok(struct nvn_vk_texcache *c, uint64_t serial, unsigned level,
                                 unsigned layer0, unsigned nlayers);
// 内容交接计数（诊断/验收）：只读，读取不清零。
struct nvn_vk_texcache_content_stats {
    uint64_t checks, gobs_scanned, gobs_changed, patches, full_covers, ledger_fail, gpu_marks, mixed,
             mixed_versioned_rejects;
    uint64_t ledger_bytes;   // 当前活条目持有的账本字节（每 512 B GOB 8 B）
    // 内容归属账本（声明写范围）的诊断：发布/应用/跳过（哈希证明未变）/被判为未解释变化而 fail-closed 的次数。
    uint64_t ranges_published, ranges_applied, range_skips, hint_unexplained, range_truncated;
};
void nvn_vk_texcache_content_stats(struct nvn_vk_texcache *c, struct nvn_vk_texcache_content_stats *out);
// 诊断/测试注入：guest 内容账本的字节上限（0 = 关闭账本，模拟超上限/分配失败）。
// 生产不调用；用来验证「账本不可用 + 有 GPU 内容」必须 fail-closed、而「全 CPU 内容」仍可整块重传。
void nvn_vk_texcache_set_ledger_max_bytes(uint64_t n);
uint64_t nvn_vk_texcache_ledger_max_bytes(void);
// 诊断：重传时因“所在页都没脏”被跳过的子资源数（mip/层）。
uint64_t nvn_vk_texcache_partial_skipped(struct nvn_vk_texcache *c);
struct nvn_vk_tex *nvn_vk_texcache_get_tex(struct nvn_vk_texcache *c, const tex_t *t, char *why, size_t why_size);
// 按注册的 NVNtextureView 取（或建）精确视图：mip/层范围、格式覆盖、target 类型都参与身份；
// 同一图像上换视图不重建图像。v 可为 NULL（等同默认整幅视图）。未知/越界/不兼容的视图明确失败。
VkImageView nvn_vk_texcache_view(struct nvn_vk_texcache *c, struct nvn_vk_tex *t, const view_t *v,
                                 char *why, size_t why_size);
// 附件视图：mip/层子集 + 合法格式覆盖（同兼容类，8 位颜色）；深度/模板覆盖明确拒绝。
// 返回的格式是附件实际使用的 VkFormat；未覆盖/默认整幅时直接复用 image 的附件视图。
VkImageView nvn_vk_texcache_attachment_view(struct nvn_vk_texcache *c, struct nvn_vk_tex *t, const view_t *v,
                                            VkFormat *format_out, char *why, size_t why_size);
// 视图级保活：拿到返回的 VkImageView 后、命令完成前 pin；flush/完成边界 unpin。
// 未注册的句柄返回 -1（调用方必须停止该笔，不能把未登记视图放进描述符）。
int nvn_vk_texcache_pin_view(struct nvn_vk_tex *t, VkImageView view);
void nvn_vk_texcache_unpin_view(struct nvn_vk_tex *t, VkImageView view);
// 视图缓存诊断/验收计数：创建、复用、LRU 淘汰（只销毁已完成的）、因全部在途而拒绝。
struct nvn_vk_tex_view_stats {
    uint64_t created, reused, evicted, pinned_full_rejects;
};
void nvn_vk_texcache_view_stats(struct nvn_vk_tex_view_stats *out);
// 丢掉这张 tex_t 对应的缓存槽（CPU 内容变了/纹理对象释放）；下一次 get_tex 重建。
void nvn_vk_texcache_drop_tex(struct nvn_vk_texcache *c, const tex_t *t);

// ================= A3：记录快照的对象身份复核 + 纹理墓碑 =================
// 记录层（hle_nvn.c 的 REC_SNAP）存的是产者写的 tex_t 快照；回放层不能只凭 magic 或副本里的
// 零字段去猜对象状态。这里给出一条明确判据和一份可查的状态归属，供附件解析（SET_RT 后的
// ClearColor / ClearDepthStencil 等消费者）在“解析不到”时给出类型化失败，而不是 no-op 或造资源。
// 判据/墓碑类型定义见 struct nvn_vk_texcache 之前。
// 返回 NVN_TEX_ID_*；why（可 NULL）在 DESC_INVALID 时写具体不自洽的字段。不读写缓存、不建资源。
int nvn_vk_texcache_tex_identity(const tex_t *t, char *why, size_t why_size);
// 查询：返回 NVN_TEX_SERIAL_*，不建资源、不改状态、不加锁（与 texcache 其余状态同一使用假设）。
// tomb 可为 NULL；RETIRED 时写入墓碑副本。生产同一时刻只有一个直连会话，未知/无会话时返回 UNKNOWN。
int nvn_vk_texcache_serial_state(uint64_t serial, struct nvn_tex_tombstone *tomb);
// 诊断：墓碑表条数/容量/被覆盖条目数。
void nvn_vk_texcache_tombstone_stats(uint32_t *n, uint32_t *cap, uint64_t *evicted);
#ifdef __cplusplus
}
#endif
void nvn_vk_texcache_mark_written(struct nvn_vk_texcache *c, VkImage image);
void nvn_vk_texcache_mark_cpu_dirty(struct nvn_vk_texcache *c, const tex_t *t);
// ---- 内容归属：CPU 声明写范围（权威；来自调用点的精确范围，不靠扫描推断）----
// 地址版：guest 存储地址 + 字节数（生产接缝 = nvnMemoryPoolFlushMappedRange 的 (mem+off, n)）。
// 与 mark_cpu_dirty 的区别：这里覆盖 GPU 内容；未被声明覆盖的字节保持图像内容。
void nvn_vk_texcache_mark_cpu_written_range(struct nvn_vk_texcache *c, const void *addr, uint64_t len);
// 矩形版（生产接缝 = nvnTextureWriteTexels 的 level/layer/区域）：按块线性布局换成精确字节范围。
// 3D 纹理 z0/nz 是深度切片，其余 target 是 layer0+z0 起的层。几何不支持的纹理不猜：不记账，
// 该路径退化为 hint（有 GPU 内容时解析层 fail-closed）。
void nvn_vk_texcache_mark_written_rect(struct nvn_vk_texcache *c, const tex_t *t, unsigned level,
                                       unsigned layer0, int x, int y, int w, int h, int z0, int nz);
// 块编码影子（压缩纹理经“块=像素”视图渲染）把字节拷回底图后：该 (mip, 层/深度切片) 子资源
// 的内容由 GPU 决定，CPU 镜像不再可信；后续命中不能拿 guest 旧字节重传。
void nvn_vk_texcache_mark_gpu_written_subres(struct nvn_vk_texcache *c, const tex_t *t, unsigned level, unsigned layer);

