// nvn_gpu_* 的 Vulkan 后端（W1 D8 第一段：接管已有设备 → 缓冲 → 写 / 填 / 拷 → 提交 / 等 → 读回 → release / abort / close）。
//
// 范围（本段只做契约用例 C01–C08 需要的原语；其余函数按契约明确返回 E_UNSUPPORTED，不静默空实现）：
//   · 设备：nvn_gpu_open 接管平台已建好的 struct s2a_vk_device（不另建、不拥有其寿命）；
//   · 缓冲：create / cpu / va / write / fill / copy / arena / use / release(retired)；
//   · 提交：cmd_begin / commit / abort / fence_done / fence_failed / fence_wait；
//   · 失败终止（A4）：第一个失败段的 signal = F，先发布 F 再以 FAILED 回调；F 之后不再执行；
//   · 发布顺序：GPU 完成 → done（核心回写）→ release 语义推进已完成值；fence_wait 以 acquire 语义读。
// 本段不做：贴图 / 视图 / 采样器 / 着色器 / 管线 / 通道 / 上屏（清屏、三角形、深度用例随后接，届时按同一契约补）。
//
// 线程模型：[提交] 线程录制并提交；后端自己的 worker 线程等栅栏、调 done / retired（[回调]）。
// 对象状态（refs / released）都由 g_mu 保护；回调一律在锁外调用。
//
// 已知与契约 v2 的接点（消费审查，见 docs/设计_回放核心共用.md / 工单 #784 评论）：
//   1. nvn_gpu_binding 的 union 只能放 view 或 samp，表达不了 TEX_SAMP 组合（安卓现有 COMBINED_IMAGE_SAMPLER）；
//   2. done.signal 的「0 = 不推进」与 commit 的「signal 严格递增」冲突；本实现按后者：signal = 0 直接 E_ARG，
//      因为失败终止用 F = 0 表示「没有失败」，无信号段一旦失败无法表达。等契约属主定稿后同批改。

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nvn_backend.h"
#include "nvn_plat.h"   // 调试开关与一行日志（上屏段的调试色、GPU 时间读数）
#include "nvn_vk_format.h"
#include "nvn_vk_texview.h"
#include "nvn_vk_bcdec.h"
#include "nvn_vk_driver_cache.h"   // 驱动管线缓存的落盘文件层（见「驱动管线缓存」一节）
#include "nvn_fmt_info.h"
#include "nvn_fmt_expand.h"
#include "vk_device.h"
#include "../capprobe/capprobe.h"
#include "nvn_vk_dq.inc"   // 两级回放：从这里往下 vkCmd* 等名字走它的包装（调试开关 nvk_dq，缺省关）
S2A_CAPPROBE_RUNTIME_LOST_WEAK_REF;   // 宿主链接单元无 bridge.cpp 时解析为 NULL

#define VK_SEG_QUEUE_INIT 16
#define VK_ARENA_MIN (256u * 1024u)
#define VK_WAIT_SLICE_NS (50ull * 1000ull * 1000ull)

enum { STAT_BUFS = 0, STAT_SUBMITS, STAT_DONE_OK, STAT_DONE_FAILED, STAT_RETIRED, STAT_WRITES, STAT_FILLS, STAT_COPIES, STAT_N };
static const char *const stat_names[STAT_N] = {
    "bufs_created", "submits", "segments_done", "segments_failed", "bufs_retired", "buf_writes", "buf_fills", "buf_copies",
};

struct nvn_gpu_buf_s {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    uint64_t bytes;
    uint64_t va;
    int mem_kind;
    int released;
    int refs;
    nvn_gpu_retired_fn retired;
    void *retired_user;
    struct nvn_gpu_buf_s *all_next;
    struct nvn_gpu_buf_s *retire_next;   // 退休队列就地链接，队列本身不再分配
};

struct nvn_gpu_tex_s {
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize mem_size; uint32_t mem_type;   // 这块显存向驱动要的大小和内存类型（还回 texmem 池时用）
    VkImageView view;
    VkFormat format, sample_format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    uint32_t w, h, depth, levels, layers, samples, nvn_format, target;
    int array_compatible;
    int view_fmt, released, refs, bc_decoded;
    VkBuffer bc_sbuf; VkDeviceMemory bc_smem;   // bc_decoded 贴图的压缩原数据影子（GPU 缓冲，按级、按层 / 3D 切片、行紧排；见 bc_shadow_layout）
    uint8_t fb_sample;   // 同一通道里既当附件又被采样过（反馈环）：这张图以后当附件一律 GENERAL（见「图像布局」）
    uint16_t swz;   // 存储自带的通道映射（tex_builder_t.swz 打包法，0 = 恒等）：视图没自己设映射时用它
    struct nvn_gpu_tex_s *all_next;
};

struct nvn_gpu_view_s {
    VkImageView view;
    struct nvn_gpu_tex_s *tex;
    VkBufferView buffer_view;
    struct nvn_gpu_buf_s *buf;
    VkFormat buffer_format;
    VkFormatFeatureFlags buffer_features;
    uint32_t buffer_caps;
    uint32_t nvn_format;   // 视图的有效 NVN 格式（可从贴图覆盖）；clear 的 is_int 按它判
    VkFormat format;       // 视图自己的原生格式：当附件时渲染通道按它声明（视图换了格式，硬件就按视图的格式写，比如经 sRGB 视图写线性存储）
    uint32_t base_level, levels, base_layer, layers;
    int released, refs;
    int swizzled;   // 通道映射非恒等：只能采样，不能当附件
    VkImageViewType type;
    VkImageAspectFlags aspect;   // 这个视图取的面（深度模板格式的采样视图只取一个面）
    uint32_t att_layer;          // 当附件时取的层 / 3D 深度切片（视图没设层范围 = 0）
    VkImageView att;             // 附件形态（一级一层的 2D 视图），第一次当附件时建（view_attachment）
    VkImageView img;             // 图像形态（恒等通道映射），第一次当存储图像时建：存储图像视图必须恒等映射（view_image_form）
    // 反查表：这个视图当过附件的帧缓冲（帧缓冲缓存槽位 + 代 + 句柄）。视图销毁时只拆这几项，
    // 不必在全局 2048 项里线性扫一遍（真机上一帧 30 个视图销毁 × 扫描全表 × 每通道开通道要拿同一把锁，开通道里光等锁就 10 微秒）。
    struct { uint16_t idx; uint32_t gen; VkFramebuffer fb; } fbs[6];
    uint8_t nfbs, fbs_overflow;   // 溢出（同一个视图用在 6 个以上帧缓冲里）：销毁时退回全表扫
    struct nvn_gpu_view_s *all_next;
};

struct nvn_gpu_shader_s {
    VkShaderModule mod;
    int missing_storage_format_feature;
    nvn_gpu_stage stage;
    nvn_gpu_layout_ent *ents;
    VkFormat *tfmt;              // 与 ents 平行：TBUF / IMAGE_TBUF 项的 texel 视图格式（自 blob 反射）；非 texel 项 UNDEFINED
    uint32_t nent;
    uint32_t push_off, push_bytes;
    char entry[32];
    int released, refs;
    struct nvn_gpu_shader_s *all_next;
};

struct nvn_gpu_pipe_s {
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkDescriptorSetLayout dsl[4];
    uint32_t nset;
    nvn_gpu_layout_ent *stage_ents[6];
    VkFormat *stage_tfmt[6];
    int8_t texel_binds;   // 有没有纹素缓冲类的绑定项：0 还没数，1 有，-1 没有（见 bind_descriptor_groups）
    uint32_t stage_nent[6];
    uint32_t stage_push_off[6], stage_push_bytes[6];
    nvn_gpu_pipe_desc key;
    struct nvn_gpu_pipe_s *all_next;
    struct nvn_gpu_pipe_s *hnext;   // 图形管线的散列桶链（g_pipe_ht）
};

struct nvn_gpu_samp_s {
    VkSampler sampler;
    VkSampler int_sampler;   // 给整数格式的视图用：最近点、不做各向异性、整数边界色（整数格式不能线性过滤）；参数本来就合规时与 sampler 是同一个
    int released, refs;
    struct nvn_gpu_samp_s *all_next;
};

struct vk_ref { void *obj; int type; };
enum { REF_BUF = 1, REF_TEX, REF_VIEW, REF_SAMP };

struct vk_staging { struct nvn_gpu_buf_s *buf; };

struct pass_rec { VkRenderPass rp; VkFramebuffer fb; };
// 本段里一张图当前在哪：第一次碰到按 GENERAL（段首约定），之后按本段记的（见「图像布局」）
struct cmd_lay { struct nvn_gpu_tex_s *t; VkImageLayout cur; };
enum { DESC_MEMO_N = 2048 };
struct desc_memo { uint64_t h1, h2; VkDescriptorSetLayout dsl; VkDescriptorSet set; };
struct bc_temp { VkImage image; VkDeviceMemory memory; struct nvn_vk_bcdec_rec rec; struct bc_temp *next; };   // rec：缓冲 → BC 贴图录进段里的解码资源（#697）
enum { TS_PASS_MAX = 250 };   // 调试读数：一段里逐个计时的通道数上限

// 冗余状态消除的计数下标（见 nvn_gpu_cmd_s.es）
enum { ES_PIPE, ES_VP, ES_SC, ES_BC, ES_ST, ES_DB, ES_VB, ES_IB, ES_PUSH, ES_KINDS };
struct nvn_gpu_cmd_s {
    VkCommandBuffer cb;
    struct bc_temp *bc_temps;
    int recording;
    int in_pass;
    // 上一道全屏障之后录过「缓冲作目标」的传输：传输只在开头带屏障，绘制有渲染通道开头的外部依赖兜着，
    // 计算派发没有，所以派发前按它补一道（开段置 1：前置段 / 上一段的传输写，段末那道屏障只对主机可见）
    int xfer_dirty;
    nvn_gpu_rc sticky;
    char why[192];
    struct vk_ref *refs;
    uint32_t nrefs, refcap;
    struct vk_staging *staging;
    uint32_t nstaging, scap;
    struct nvn_gpu_buf_s *arena;
    uint64_t arena_used, arena_cap;
    VkCommandPool pool;
    VkDescriptorPool dpool;                  // 当前在分配的描述符池
    VkDescriptorPool *dpools_full;           // 已经分满的池：里面的组还被已录制的命令引用，段完成时一并销毁
    uint32_t ndpools_full, dpools_full_cap;
    struct nvn_gpu_pipe_s *pipe;
    struct pass_rec *passes;
    struct desc_memo *memo;   // 这一段里写过的描述符组：内容 → 组（见 bind_descriptor_groups）；懒分配，随段释放
    uint32_t memo_n;
    uint32_t npasses, passcap;
    uint32_t pass_w, pass_h;
    uint32_t pass_ncolor, pass_layers;       // 当前通道的颜色槽数、层数（clear_rect 用）
    uint32_t pass_color_mask;                // 哪些颜色槽有附件（位 i = 槽 i；空位为 0）
    uint32_t pass_cfmt[8], pass_dsfmt, pass_samples;   // 当前通道各槽的 NVN 格式（空位 0）、深度模板格式、采样数（带掩码的清除建管线用）
    VkImageAspectFlags pass_ds_aspect;       // 当前通道深度模板附件有的面（0 = 没有该附件）
    VkQueryPool tsq;                         // 调试读数 nvk_gputime：段首尾各一个 GPU 时间戳，后面每个通道首尾各一个（没开就是空）
    struct { uint16_t w, h, draws; uint8_t ncol, ds; } tsp[TS_PASS_MAX];
    uint32_t ntsp;
    VkCommandBuffer init_cb;                 // 本段提交时带上的「新建图像转 GENERAL」命令缓冲（没有就是空）
    struct nvn_gpu_tex_s **newimg;           // 上面那个命令缓冲转的图像（各持一份引用，段完成时归还；段没执行则放回待转表）
    uint32_t n_newimg;
    struct cmd_lay *lay;   // 本段内每张图当前的布局（段首一律 GENERAL，段末转回 GENERAL）
    uint32_t nlay, laycap;
    int opt;                        // 本段是否走对齐语义的布局（开段时读 debug.s2a.nvk_opt_layout，缺省 0）
    nvn_gpu_pass_desc pass_desc;    // 当前通道的原始描述（反馈环拆通道时按它重开）
    struct nvn_gpu_tex_s *pass_tex[9];   // 当前通道各附件的贴图（pass_natt 个；拆通道判断反馈环用）
    uint32_t pass_natt;
    nvn_gpu_dyn dyn;
    int has_dyn;
    struct { nvn_gpu_buf buf; uint64_t off; } vbuf[16];
    struct { nvn_gpu_binding b; } binds[6][64];   // 每阶段一张表，bind 只替换该阶段
    uint32_t nbinds[6];
    VkDescriptorSet desc_sets[4];   // 仅本命令段；旧组仍归本段的池所有，不就地覆盖在途绑定
    VkPipelineLayout desc_layout;
    uint32_t desc_dirty;
    // 描述符写入影子（开关 nvk_desc_skip，缺省开）：同一个组对象里，内容和上次真正写进去的一样就不再写。
    // 真机上一帧约 1030 次整组重建、每次 11 条描述符；其中大部分条目（贴图句柄、采样器、常量缓冲片）和上一笔相同。
    struct desc_shadow { uint64_t a, b, c; uint32_t type; uint16_t binding; uint8_t valid; } dsh[4][32];
    VkDescriptorSet dsh_set[4];
    uint8_t dsh_ok[4];
    uint8_t desc_point;             // 当前这几组绑在哪个绑定点：0 还没绑，1 图形，2 计算

    uint8_t *push_data[6];        // 各阶段 push 暂存；vkCmdPushConstants 录制时拷贝入命令缓冲，段完成即放
    uint32_t push_capacity[6]; // 分配字节数，测试与管线切换共用
    uint8_t push_have[6];
    uint32_t push_gen[6];         // 各阶段 push 内容的代号：nvn_gpu_push 写进的字节与原先不同才加一（发命令时据此判断要不要重发）
    // 冗余状态消除（#708）：上次真正录进命令缓冲的绘制状态。只在发命令那一刻和它比，相同就不再录；
    // ok 位为 0 = 不知道命令缓冲里现在是什么（命令对象新建时全 0；通道开始、矩形清除、计算派发之后整份作废）。
    // 只管管线、动态状态、顶点 / 索引缓冲、push 常量；描述符组不在这里（#657）。
    struct {
        VkPipeline gpipe;                                  // 绑着的图形管线（空 = 不知道）
        uint8_t vp_ok, sc_ok, bc_ok, st_ok, db_ok, ib_ok;
        VkViewport vp; VkRect2D sc; float bc[4]; uint32_t st[2][3]; float db[3];
        uint16_t vb_ok; VkBuffer vb[16]; VkDeviceSize vboff[16];
        VkBuffer ib; VkDeviceSize ioff; VkIndexType it;
        uint8_t push_ok[6]; VkPipelineLayout push_layout[6]; uint32_t push_off[6], push_bytes[6], push_gen[6];
    } es;
    uint32_t es_count[ES_KINDS];   // 实际录了几次（测试读数：nvn_gpu_vk_test_emit_counts）
    // 遮挡查询（nvn_gpu_cmd_desc.occlusion_slots > 0 才有）：一段一个查询池，段开头整池重置；每个槽一段里只能用一次
    VkQueryPool qpool; uint32_t qcount, qmax_used; int32_t q_active; uint8_t *q_used; uint8_t pass_occl;
    struct nvn_gpu_cmd_s *live_next;
};

// 冗余状态消除：影子整份作废（之后每一项第一次用到都照发）
static void es_reset(struct nvn_gpu_cmd_s *c) { memset(&c->es, 0, sizeof c->es); }

struct vk_segment {
    struct vk_segment *next;
    struct nvn_gpu_cmd_s *cmd;
    uint64_t signal;
    nvn_gpu_done_fn done;
    void *user;
    VkFence fence;
    VkCommandBuffer cb;
    int failed;
    int submitted;
    int executed;
    int internal;   // 后端自己的段（呈现）：不占时间线上的值，完成后只回收
    nvn_gpu_rc rc;
    char why[192];
};

struct nvn_gpu_vk {
    struct s2a_vk_device *dev;
    struct nvn_vk_bcdec *bc;
    struct bc_temp *bc_pending;  // 解码已提交但未证明完成，不能随失败的命令段释放
    VkDevice d;
    VkPhysicalDevice phys;
    VkQueue queue;
    nvn_gpu_caps caps;
    pthread_t worker;
    int worker_started;
    int closing;
    int closed;
    _Atomic int stop;
    int busy;
    int commit_active;   // 正在 commit 的线程数：close 要等它归零才拆资源
    int close_active;    // 已有一个线程在跑 close，防止重复 join
    _Atomic int device_lost;
    uint64_t completed;
    uint64_t failed;
    uint64_t last_signal;
    uint64_t *submitted;
    uint32_t nsub, capsub;
    struct vk_segment *qhead, *qtail;
    struct nvn_gpu_buf_s *rhead, *rtail;
    struct nvn_gpu_cmd_s *live;
    struct nvn_gpu_buf_s *bufs;
    struct nvn_gpu_tex_s *texs;
    struct nvn_gpu_view_s *views;
    struct nvn_gpu_samp_s *samps;
    struct nvn_gpu_shader_s *shaders;
    struct nvn_gpu_pipe_s *pipes;
    uint64_t stats[STAT_N];
    int fault_inject;
    uint64_t test_fail_signal;    // 测试钩子：让指定 signal 的段表现为 fence 失败
    uint64_t test_delay_signal;   // 测试钩子：让指定 signal 的段先睡 ms
    uint32_t test_delay_ms;
};

static struct nvn_gpu_vk g;
static _Atomic int g_open;   // 跨线程：close 置 0 后其他线程据此拒收
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// 诊断读数（nvk_gputime 开着时每 300 次上屏打一行）：每帧新建了多少个驱动对象、写了多少次描述符、开了多少个查询。
enum { OC_RP, OC_FB, OC_CPOOL, OC_DPOOL, OC_DALLOC, OC_DWRITE, OC_QUERY, OC_VIEW, OC_N };
static uint64_t g_oc[OC_N], g_oc_total[OC_N];
static uint64_t g_acq_ns;   // 取窗口图（vkAcquireNextImageKHR）累计用时，随对象读数那一行打
#define OC(i) (__atomic_fetch_add(&g_oc[i], 1, __ATOMIC_RELAXED), __atomic_fetch_add(&g_oc_total[i], 1, __ATOMIC_RELAXED))
uint64_t nvn_vk_dbg_created(int which) { return which >= 0 && which < OC_N ? __atomic_load_n(&g_oc_total[which], __ATOMIC_RELAXED) : 0; }   // 用例读：会话以来新建的各类对象数（0 渲染通道 1 帧缓冲 …，见上面的枚举）
// ---- 通道对象缓存：渲染通道对象与帧缓冲跨帧保留 ----
// 原先每开一个通道都 vkCreateRenderPass + vkCreateFramebuffer、所在的段做完再销毁；一帧一百多个通道，目标就那么十几组，每帧重复建销。
// 做法照 d2r-mobile 安卓笔记 perf-round2 的发现 3：渲染通道对象按「附件描述」记（与具体图像无关，会话结束才销毁）；
// 帧缓冲按「渲染通道 + 各附件视图 + 尺寸」记，直到其中任何一个视图销毁。视图要等引用它的段都做完才会销毁（cmd_ref 保活），
// 所以那时用到这个帧缓冲的命令也都做完了，可以当场销毁。表满了的那一个照旧路走（记在段上、段做完销毁）。
// 调试开关 nvk_pass_cache=0 关掉（缺省开）。
static void pc_drop_view(VkImageView v);
static void pc_drop_view_fast(struct nvn_gpu_view_s *view, VkImageView v);   // 视图销毁：按反查表只拆它当过的帧缓冲
static void pc_fb_note_desc(const nvn_gpu_pass_desc *d, int slot, uint32_t gen, VkFramebuffer fb);   // 记「这些附件视图当过这个帧缓冲」
static void pc_destroy_all(void);
enum { PIPE_HT_N = 1024 };
static uint64_t g_desc_ns[4], g_desc_nw, g_desc_sets, g_desc_hit;   // 描述符四步：分配 / 构造写入项 / 写入 / 绑定；写入的描述符条数；重写的组数
// 诊断读数（nvk_gputime 开着才计）：贴图 / 视图 / 帧缓冲的建与拆各用多久、各几次。
// 0 vkCreateImage，1 要显存并绑定，2 默认视图，3 帧缓冲，4 附件视图，5 拆视图，6 从帧缓冲表里摘（pc_drop_view），7 拆贴图（视图 + 图 + 还显存）
static uint64_t g_life_ns[8], g_life_n[8];
#define LIFE_T0 uint64_t lt_ = g_draw_tm ? now_ns() : 0
#define LIFE_ADD(i) do { if (g_draw_tm) { uint64_t ln_ = now_ns(); __atomic_fetch_add(&g_life_ns[i], ln_ - lt_, __ATOMIC_RELAXED); __atomic_fetch_add(&g_life_n[i], 1, __ATOMIC_RELAXED); lt_ = ln_; } } while (0)
static uint64_t now_ns(void);
static int g_draw_tm; static uint64_t g_draw_ns[3];   // 绘制入口分段计时：描述符 / 状态 / 顶点索引与绘制调用
static void draw_tm_init(void) { if (!g_draw_tm) g_draw_tm = nvn_plat_flag("nvk_gputime", 0) != 0; }   // 与 GPU 时间戳同一个开关
static uint64_t g_pl_ns, g_pl_n, g_pl_steps;   // 取图形管线：命中查找的累计纳秒 / 次数 / 比过的条数
static struct nvn_gpu_pipe_s *g_pipe_ht[PIPE_HT_N];   // 图形管线的散列桶（见 pipe_hash）；持 g_mu
// ---- 驱动管线缓存（#822）：后端建的每条管线都带同一个 VkPipelineCache；会话收尾把驱动给的数据写进 open_desc.cache_dir，
// 下一局开会话时读回来喂给驱动。真机每局起局二十多秒的同步建管线（每条二三十毫秒）是驱动每次从头编，带上缓存后重开一局按内容命中。
// 契约（nvn_backend.h open_desc.cache_dir）：NULL = 不落盘；后端有界维护、收尾时写、不逐帧写盘。文件层是旧回放用的 nvn_vk_driver_cache
// （魔数 + 版本 + SHA-256、临时文件 fsync 后原子替换、坏了当没有、64 MiB 封顶）。
// 缓存只是加速，不是前提：读 / 建 / 取数据 / 写，任何一步失败都只记一行日志，管线照旧能建（不拦用户）。
// 调试开关 nvk_pipe_cache=0 关掉（缺省开）：不建缓存、不读不写盘，管线照旧不带缓存建。
// 局中补写（#868，「收尾时写」之外的第二个时机）：被系统杀掉 / 崩掉的局走不到收尾，驱动编出来的结果就白丢了——所以建管线累计够多、隔得够久时，
// 在局中取一份驱动缓存交给后台线程落盘，每次落盘仍是整份原子替换（见下面 plc_save_midrun）。不是逐帧写盘：缺省最多 30 秒一次，数据没变不写。
static VkPipelineCache g_plc;                // VK_NULL_HANDLE = 没有（开关关了 / 驱动建不出）：建管线时照传，空句柄就是不带缓存
static char g_plc_dir[480];                  // 落盘目录副本；空 = 不落盘（只在本局内复用，收尾不写）
static uint8_t g_plc_uuid[VK_UUID_SIZE];     // 设备 + 驱动的管线缓存标识（文件名里带它，换驱动不互读）
static size_t g_plc_loaded;                  // 开会话时读到并交给驱动的字节数（0 = 没读到）
static uint64_t g_plc_rejected;              // 本会话里旧文件读不出 / 驱动不收、被丢掉的次数
static uint64_t g_plc_made;                  // 本会话经缓存建出的管线条数（图形 + 计算 + 清屏）
// ---- 局中补写的状态。触发：g_plc_made 比上次保存时多 ≥ N 条（nvk_plc_new，缺省 64），且头一次距开会话 ≥ F 毫秒（nvk_plc_first_ms，10000）、
// 之后距上次保存 ≥ E 毫秒（nvk_plc_every_ms，30000）；取到的数据大小与上次保存（或开局读入）的一致就不写。
// 分工：取数据（vkGetPipelineCacheData，真机约 0.5 ms）在触发的那条线程上做；写盘（临时文件 + fsync + 原子替换，真机十几毫秒、慢盘更久）在后台线程做，
// 后台线程只拿自己的缓冲区和路径，不碰任何 Vulkan 句柄、不碰会话全局。同一时刻最多一次写在飞（进程级、不排队：在飞时新的触发直接跳过）。
// 收尾：等在飞的写算进 close 的截止时间，到点没写完就放弃——不再写最终份、不再等它，旧文件原样；写失败 / 设备丢失只记日志。
static uint64_t g_plc_t_open;                // 开会话的时刻（单调时钟 ns）
static uint64_t g_plc_t_saved;               // 上一次局中保存（含「取了数据发现没变」）的时刻；0 = 本局还没有过。原子读写
static uint64_t g_plc_made_saved;            // 那一次的 g_plc_made。原子读写
static size_t g_plc_size_saved;              // 上一次落盘（或开局读入）的数据字节数：取到的数据与它一致就不写。持 g_plc_mu
static uint64_t g_plc_first_ns, g_plc_every_ns, g_plc_thr_new;   // 触发的三个阈值：开会话时读一次
static pthread_mutex_t g_plc_mu = PTHREAD_MUTEX_INITIALIZER;     // 护 g_plc 的销毁与 g_plc_size_saved：触发取数据 vs 收尾；触发用 trylock，拿不到就让给别人
static int g_plcw_busy;                      // 后台写在飞（进程级，跨会话；原子读写）
static int g_plcw_failed;                    // 最近一次后台写失败了：收尾时即使大小没变也再写一次。原子读写
static uint64_t g_plcw_started, g_plcw_ok, g_plcw_fail, g_plcw_same;   // 读数（进程累计，不随会话清零）：交给后台写的次数 / 成功 / 失败 / 与上次一致没写
static pthread_mutex_t g_submit_mu = PTHREAD_MUTEX_INITIALIZER; // 提交串行化：校验 signal、vkQueueSubmit、入队
static pthread_cond_t g_cv;
static int g_cv_ready;
static _Thread_local int g_in_callback;   // [回调] 线程：只许调 [任意] 函数

// ---------------------------------------------------------------- 小工具

static nvn_gpu_rc rc(int code) { nvn_gpu_rc r; r.code = code; return r; }
static int rc_is(nvn_gpu_rc r, int code) { return r.code == code; }

static nvn_gpu_rc fail(nvn_gpu_err *e, int code, const char *fmt, ...) {
    if (e) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->why, sizeof e->why, fmt, ap);
        va_end(ap);
        e->rc = rc(code);
    }
    return rc(code);
}

static void set_sticky(struct nvn_gpu_cmd_s *c, int code, const char *fmt, ...) {
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;   // 首错原文
    c->sticky = rc(code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof c->why, fmt, ap);
    va_end(ap);
}

static uint64_t add_sat(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void deadline_after(struct timespec *ts, uint64_t ns) {
    uint64_t t = add_sat(now_ns(), ns);
    ts->tv_sec = (time_t)(t / 1000000000ull);
    ts->tv_nsec = (long)(t % 1000000000ull);
}

// ---------------------------------------------------------------- 提交集合

static int submitted_has(struct nvn_gpu_vk *vk, uint64_t v) {
    uint32_t lo = 0, hi = vk->nsub;   // signal 严格递增，数组天然有序
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (vk->submitted[mid] == v) return 1;
        if (vk->submitted[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void submitted_compact_locked(struct nvn_gpu_vk *vk) {
    uint32_t drop = 0;
    while (drop < vk->nsub && vk->submitted[drop] <= vk->completed) drop++;
    if (!drop) return;
    memmove(vk->submitted, vk->submitted + drop, (size_t)(vk->nsub - drop) * sizeof *vk->submitted);
    vk->nsub -= drop;
}

static int submitted_add(struct nvn_gpu_vk *vk, uint64_t v) {
    if (submitted_has(vk, v)) return 1;
    if (vk->nsub == vk->capsub) {
        uint32_t cap = vk->capsub ? vk->capsub * 2 : VK_SEG_QUEUE_INIT;
        uint64_t *p = realloc(vk->submitted, (size_t)cap * sizeof *p);
        if (!p) return 0;
        vk->submitted = p;
        vk->capsub = cap;
    }
    vk->submitted[vk->nsub++] = v;
    return 1;
}

// ---------------------------------------------------------------- 引用与退休

// 调用方持 g_mu。对象本身就是队列节点：分配失败不可能吞掉 retired。
static void queue_retire_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    b->retire_next = NULL;
    if (vk->rtail) vk->rtail->retire_next = b;
    else vk->rhead = b;
    vk->rtail = b;
    pthread_cond_broadcast(&g_cv);
}

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p);
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t);
static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v);
static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s);
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type);

// ---------- 图像布局 ----------
// 两个取值（开关 debug.s2a.nvk_opt_layout）：
//   · 0（缺省，旧行为）：图像常驻 VK_IMAGE_LAYOUT_GENERAL：通道附件按 GENERAL 进出，采样 / 存储按 GENERAL 绑，
//     传输在同一次调用里转去转回。
//   · 1：附件在通道里用 COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL，采样用
//     SHADER_READ_ONLY_OPTIMAL / DEPTH_STENCIL_READ_ONLY_OPTIMAL，存储图像与传输照旧 GENERAL / TRANSFER_*。
//     移动 GPU 只有附件布局下才开帧缓冲压缩与快速解析，GENERAL 下用不了：一帧几十个通道的固定成本主要花在这上面。
// 段与段之间的静止布局一律 GENERAL：每张图在本段的第一次迁移都从 GENERAL 出发，段末（commit 前）把本段动过的都转回
// GENERAL（lay_rest_at_commit）。这样命令缓冲里录的 oldLayout 只依赖本段自己的录制顺序，和「哪一段先提交」无关——
// 前置段 g_pre 是录制在后、提交在前，靠全局记布局遇到这种次序就记不准，旧实现因此一律钉 GENERAL。
// 新建的图像（UNDEFINED）仍由「创建之后的第一次提交」先于那一段转成 GENERAL（submit_cbs 的 init_cb）：
// 提交时才知道谁先跑，预知不了首次用途，所以新图首用比最优多一次转换。
// 正确性红线：同一张图在同一通道里既当附件又被采样（反馈环）时通道内不能改附件布局；绘制前发现这种绑定就结束通道、
// 把这张图转成 GENERAL 再按 LOAD 重开（内容留在显存里），并把这张图标成 fb_sample：以后当附件一律 GENERAL。
// 声明了遮挡查询的通道（pass_desc.occlusion）中途不能拆（查询不能跨通道重开），这种通道附件一律 GENERAL。
static struct nvn_gpu_tex_s **g_newimg;   // g_mu
static uint32_t g_newimg_n, g_newimg_cap;
static int newimg_add_locked(struct nvn_gpu_tex_s *t) {
    if (g_newimg_n == g_newimg_cap) {
        uint32_t cap = g_newimg_cap ? g_newimg_cap * 2 : 64;
        struct nvn_gpu_tex_s **p = realloc(g_newimg, (size_t)cap * sizeof *p);
        if (!p) return 0;
        g_newimg = p; g_newimg_cap = cap;
    }
    g_newimg[g_newimg_n++] = t;
    return 1;
}
static void newimg_del_locked(struct nvn_gpu_tex_s *t) {
    for (uint32_t i = 0; i < g_newimg_n; i++) if (g_newimg[i] == t) { g_newimg[i] = g_newimg[--g_newimg_n]; return; }
}
static struct cmd_lay *lay_get(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, int make) {
    for (uint32_t i = 0; i < c->nlay; i++) if (c->lay[i].t == t) return &c->lay[i];
    if (!make) return NULL;
    if (c->nlay == c->laycap) {
        uint32_t cap = c->laycap ? c->laycap * 2 : 32;
        struct cmd_lay *p = realloc(c->lay, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        c->lay = p; c->laycap = cap;
    }
    c->lay[c->nlay].t = t;
    c->lay[c->nlay].cur = VK_IMAGE_LAYOUT_GENERAL;
    return &c->lay[c->nlay++];
}
static VkImageLayout lay_cur(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    struct cmd_lay *e = lay_get(c, t, 0);
    return e ? e->cur : VK_IMAGE_LAYOUT_GENERAL;
}
// 附件这一段用什么布局（只看这一张图与通道本身）
static VkImageLayout pass_att_layout(const nvn_gpu_pass_desc *d, const struct nvn_gpu_tex_s *t, int depth, int opt) {
    if (!opt || t->fb_sample || d->occlusion || (!depth && d->fb_fetch)) return VK_IMAGE_LAYOUT_GENERAL;
    return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
// 采样这张图要用哪个布局：深度模板只有 DEPTH_STENCIL_READ_ONLY 这一种只读布局
static VkImageLayout sample_layout_for(const struct nvn_gpu_tex_s *t) {
    return (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
// 绑定项里的贴图（采样 / 存储图像；别的类别没有图）
static struct nvn_gpu_tex_s *bind_tex_of(const nvn_gpu_binding *b) {
    struct nvn_gpu_view_s *v = b->cls == NVN_GPU_RES_TEX ? b->u.view
                              : b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view
                              : b->cls == NVN_GPU_RES_IMAGE ? b->u.view : NULL;
    return v ? v->tex : NULL;
}

// 命令段用完的命令池（带它那一个命令缓冲）和描述符池留下来给后面的段用，不再每段新建、段完成就销毁。
// 真机取样（一款 3D 游戏，每帧 9 个命令段）：回放线程约 14% 的时间在驱动向内核要显存（kgsl_sharedmem_alloc → ioctl + mmap），
// 开命令段 nvn_gpu_cmd_begin 一项占 6%；每帧新建 9.2 个命令池、5.0 个描述符池。
// 段做完后：命令池、描述符池各放进一张空闲表；开段 / 要描述符池时先从表里取，取出来再重置（vkResetCommandPool / vkResetDescriptorPool）。
// 重置之后上一段分出去的描述符组句柄全部失效——段内的影子（desc_sets、备忘表）都随段对象一起没了，不会带到下一段。
// 表有上限（多出来的照旧销毁），会话收尾全部销毁。诊断开关 nvk_seg_reuse=0 关掉（同一局里对照用），缺省开。
enum { SEGPOOL_CMD_N = 32, SEGPOOL_DESC_N = 64 };
static struct { VkCommandPool pool; VkCommandBuffer cb; } g_seg_cmd[SEGPOOL_CMD_N];
static VkDescriptorPool g_seg_desc[SEGPOOL_DESC_N];
static uint32_t g_seg_ncmd, g_seg_ndesc; static uint64_t g_seg_hit_cmd, g_seg_hit_desc;
static int g_seg_closed;   // 会话收尾已经清过表：之后再还回来的一律直接销毁（持 g_seg_mu 读写）
static pthread_mutex_t g_seg_mu = PTHREAD_MUTEX_INITIALIZER;
static int seg_reuse_on(void) { static int cur = -1; static uint32_t k; if (cur < 0 || !(++k & 63)) cur = nvn_plat_flag("nvk_seg_reuse", 1) != 0; return cur; }
static int seg_take_cmd(VkCommandPool *pool, VkCommandBuffer *cb) {
    int ok = 0;
    pthread_mutex_lock(&g_seg_mu);
    if (g_seg_ncmd) { g_seg_ncmd--; *pool = g_seg_cmd[g_seg_ncmd].pool; *cb = g_seg_cmd[g_seg_ncmd].cb; g_seg_hit_cmd++; ok = 1; }
    pthread_mutex_unlock(&g_seg_mu);
    // 重置放在取用的这一刻、由要用它的线程做：放回时（完成线程）就重置的话，两条线程会在驱动里抢同一把锁，
    // 真机上「交后端」从 0.4 涨到 1.5–2.2 毫秒 / 帧
    if (ok && vkResetCommandPool(g.d, *pool, 0) != VK_SUCCESS) { vkDestroyCommandPool(g.d, *pool, NULL); *pool = VK_NULL_HANDLE; *cb = VK_NULL_HANDLE; ok = 0; }
    return ok;
}
static void seg_give_cmd(VkCommandPool pool, VkCommandBuffer cb) {   // 这一段已经做完（或被放弃），池里没有在用的命令缓冲
    if (!pool) return;
    int kept = 0;
    if (cb && seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_ncmd < SEGPOOL_CMD_N) { g_seg_cmd[g_seg_ncmd].pool = pool; g_seg_cmd[g_seg_ncmd].cb = cb; g_seg_ncmd++; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyCommandPool(g.d, pool, NULL);   // 池同时回收它的命令缓冲
}
static VkDescriptorPool seg_take_desc(void) {
    VkDescriptorPool p = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_seg_mu);
    if (g_seg_ndesc) { p = g_seg_desc[--g_seg_ndesc]; g_seg_hit_desc++; }
    pthread_mutex_unlock(&g_seg_mu);
    if (p && vkResetDescriptorPool(g.d, p, 0) != VK_SUCCESS) { vkDestroyDescriptorPool(g.d, p, NULL); p = VK_NULL_HANDLE; }   // 同上：取用时重置
    return p;
}
static void seg_give_desc(VkDescriptorPool p) {
    if (!p) return;
    int kept = 0;
    if (seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_ndesc < SEGPOOL_DESC_N) { g_seg_desc[g_seg_ndesc++] = p; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyDescriptorPool(g.d, p, NULL);
}
// 同一张回收的账里再加两样（真机取样：池回收之后回放线程还有约 4.5% 在 nvn_gpu_buf_create → vkAllocateMemory、约 2.5% 在 vkCreateQueryPool）：
// · 命令段的暂存缓冲（arena 的块，多数是 VK_ARENA_MIN 那么大）：整个缓冲对象留下，大小相同的直接再用。它一直留在 g.bufs 链里，
//   会话收尾遍历那条链时一起销毁，所以清表只清计数。暂存区是先写后用的，旧内容不用清。
// · 遮挡查询池：槽数相同的再用；开段时照旧整池重置。
enum { SEGPOOL_STG_N = 32, SEGPOOL_Q_N = 16 };
static struct nvn_gpu_buf_s *g_seg_stg[SEGPOOL_STG_N]; static uint32_t g_seg_nstg;
static struct { VkQueryPool q; uint32_t n; } g_seg_q[SEGPOOL_Q_N]; static uint32_t g_seg_nq;
static struct nvn_gpu_buf_s *seg_take_stg(uint64_t bytes) {
    struct nvn_gpu_buf_s *b = NULL;
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_nstg; i++) if (g_seg_stg[i]->bytes == bytes) { b = g_seg_stg[i]; g_seg_stg[i] = g_seg_stg[--g_seg_nstg]; break; }
    pthread_mutex_unlock(&g_seg_mu);
    return b;
}
static int seg_give_stg(struct nvn_gpu_buf_s *b) {   // 1 = 留下了；0 = 没留，调用方照旧销毁
    int kept = 0;
    if (!seg_reuse_on() || !b->mapped) return 0;
    pthread_mutex_lock(&g_seg_mu);
    if (!g_seg_closed && g_seg_nstg < SEGPOOL_STG_N) { g_seg_stg[g_seg_nstg++] = b; kept = 1; }
    pthread_mutex_unlock(&g_seg_mu);
    return kept;
}
static VkQueryPool seg_take_q(uint32_t n) {
    VkQueryPool q = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_nq; i++) if (g_seg_q[i].n == n) { q = g_seg_q[i].q; g_seg_q[i] = g_seg_q[--g_seg_nq]; break; }
    pthread_mutex_unlock(&g_seg_mu);
    return q;
}
static void seg_give_q(VkQueryPool q, uint32_t n) {
    if (!q) return;
    int kept = 0;
    if (n && seg_reuse_on()) {
        pthread_mutex_lock(&g_seg_mu);
        if (!g_seg_closed && g_seg_nq < SEGPOOL_Q_N) { g_seg_q[g_seg_nq].q = q; g_seg_q[g_seg_nq].n = n; g_seg_nq++; kept = 1; }
        pthread_mutex_unlock(&g_seg_mu);
    }
    if (!kept) vkDestroyQueryPool(g.d, q, NULL);
}
static void seg_drain(void) {
    pthread_mutex_lock(&g_seg_mu);
    for (uint32_t i = 0; i < g_seg_ncmd; i++) vkDestroyCommandPool(g.d, g_seg_cmd[i].pool, NULL);
    for (uint32_t i = 0; i < g_seg_ndesc; i++) vkDestroyDescriptorPool(g.d, g_seg_desc[i], NULL);
    for (uint32_t i = 0; i < g_seg_nq; i++) vkDestroyQueryPool(g.d, g_seg_q[i].q, NULL);
    g_seg_ncmd = g_seg_ndesc = 0; g_seg_nq = 0; g_seg_nstg = 0; g_seg_closed = 1;   // 暂存缓冲在 g.bufs 链里，由收尾遍历销毁
    pthread_mutex_unlock(&g_seg_mu);
}

// 贴图显存块的小池。动态分辨率的游戏每帧换渲染目标的宽高，每张新图都向驱动要一块显存、旧图拆的时候还回去：
// 一款 3D 游戏每帧 10 张时「要显存并绑定」1.24 毫秒 + 拆 0.41 毫秒（直接计时）。拆贴图时把显存块留在池里，
// 新图要的大小落在某块的 [块的 4/5, 块] 之间、内存类型相同就直接绑它（图已经拆了的显存块可以再绑给别的图）。
// 池按先进先出，最多 TEXMEM_N 块、TEXMEM_BYTES 字节，超了把最老的还给驱动；会话收尾全部还掉。
// 诊断开关 nvk_texmem=0 关掉（同一局里对照用），缺省开。
enum { TEXMEM_N = 48 };
#define TEXMEM_BYTES (256ull << 20)
static struct { VkDeviceMemory m; VkDeviceSize size; uint32_t type; } g_texmem[TEXMEM_N];
static uint32_t g_texmem_n; static VkDeviceSize g_texmem_bytes; static uint64_t g_texmem_hit, g_texmem_miss;
static pthread_mutex_t g_texmem_mu = PTHREAD_MUTEX_INITIALIZER;
static int texmem_on(void) { static int cur = -1; static uint32_t k; if (cur < 0 || !(++k & 63)) cur = nvn_plat_flag("nvk_texmem", 1) != 0; return cur; }
static VkDeviceMemory texmem_take(VkDeviceSize need, uint32_t type, VkDeviceSize *size) {
    VkDeviceMemory m = VK_NULL_HANDLE; int best = -1;
    pthread_mutex_lock(&g_texmem_mu);
    for (uint32_t i = 0; i < g_texmem_n; i++)
        if (g_texmem[i].type == type && g_texmem[i].size >= need && g_texmem[i].size - g_texmem[i].size / 5 <= need &&
            (best < 0 || g_texmem[i].size < g_texmem[best].size)) best = (int)i;
    if (best >= 0) { m = g_texmem[best].m; *size = g_texmem[best].size; g_texmem_bytes -= g_texmem[best].size;
        memmove(&g_texmem[best], &g_texmem[best + 1], (g_texmem_n - (uint32_t)best - 1) * sizeof g_texmem[0]); g_texmem_n--; g_texmem_hit++; }
    else g_texmem_miss++;
    pthread_mutex_unlock(&g_texmem_mu);
    return m;
}
static void texmem_give(VkDevice d, VkDeviceMemory m, VkDeviceSize size, uint32_t type) {
    if (!m) return;
    if (!size || size > TEXMEM_BYTES / 4 || !texmem_on()) { vkFreeMemory(d, m, NULL); return; }
    VkDeviceMemory out[TEXMEM_N + 1]; uint32_t nout = 0;
    pthread_mutex_lock(&g_texmem_mu);
    while (g_texmem_n && (g_texmem_n == TEXMEM_N || g_texmem_bytes + size > TEXMEM_BYTES)) {   // 最老的在前
        out[nout++] = g_texmem[0].m; g_texmem_bytes -= g_texmem[0].size;
        memmove(&g_texmem[0], &g_texmem[1], (g_texmem_n - 1) * sizeof g_texmem[0]); g_texmem_n--; }
    g_texmem[g_texmem_n].m = m; g_texmem[g_texmem_n].size = size; g_texmem[g_texmem_n].type = type; g_texmem_n++; g_texmem_bytes += size;
    pthread_mutex_unlock(&g_texmem_mu);
    for (uint32_t i = 0; i < nout; i++) vkFreeMemory(d, out[i], NULL);
}
static void texmem_drain(VkDevice d) {
    pthread_mutex_lock(&g_texmem_mu);
    for (uint32_t i = 0; i < g_texmem_n; i++) vkFreeMemory(d, g_texmem[i].m, NULL);
    g_texmem_n = 0; g_texmem_bytes = 0;
    pthread_mutex_unlock(&g_texmem_mu);
}

// 调用方持 g_mu。贴图 / 视图没有 retired 回调，最后一次引用结束即可销毁。
static void destroy_tex_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t) {
    newimg_del_locked(t);
    struct nvn_gpu_tex_s **pp = &vk->texs;
    while (*pp && *pp != t) pp = &(*pp)->all_next;
    if (*pp) *pp = t->all_next;
    LIFE_T0;
    pc_drop_view(t->view); LIFE_ADD(6);
    if (t->view) vkDestroyImageView(vk->d, t->view, NULL);
    if (t->image) vkDestroyImage(vk->d, t->image, NULL);
    texmem_give(vk->d, t->memory, t->mem_size, t->mem_type);
    LIFE_ADD(7);
    if (t->bc_sbuf) vkDestroyBuffer(vk->d, t->bc_sbuf, NULL);
    if (t->bc_smem) vkFreeMemory(vk->d, t->bc_smem, NULL);
    free(t);
}

static void destroy_view_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_view_s *v) {
    struct nvn_gpu_view_s **pp = &vk->views;
    while (*pp && *pp != v) pp = &(*pp)->all_next;
    if (*pp) *pp = v->all_next;
    LIFE_T0;
    pc_drop_view_fast(v, v->att); pc_drop_view_fast(v, v->img); pc_drop_view_fast(v, v->view); LIFE_ADD(6);
    if (v->att) vkDestroyImageView(vk->d, v->att, NULL);
    if (v->img) vkDestroyImageView(vk->d, v->img, NULL);
    if (v->view) vkDestroyImageView(vk->d, v->view, NULL);
    LIFE_ADD(5);
    if (v->buffer_view) vkDestroyBufferView(vk->d, v->buffer_view, NULL);
    struct nvn_gpu_tex_s *t = v->tex;
    struct nvn_gpu_buf_s *b = v->buf;
    free(v);
    if (b) unref_locked(vk, b, REF_BUF);
    if (t) {
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) destroy_tex_locked(vk, t);
    }
}

static void destroy_samp_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_samp_s *s) {
    struct nvn_gpu_samp_s **pp = &vk->samps;
    while (*pp && *pp != s) pp = &(*pp)->all_next;
    if (*pp) *pp = s->all_next;
    if (s->int_sampler && s->int_sampler != s->sampler) vkDestroySampler(vk->d, s->int_sampler, NULL);
    if (s->sampler) vkDestroySampler(vk->d, s->sampler, NULL);
    free(s);
}

static int ref_released(void *obj, int type) {
    switch (type) {
    case REF_BUF: return ((struct nvn_gpu_buf_s *)obj)->released;
    case REF_TEX: return ((struct nvn_gpu_tex_s *)obj)->released;
    case REF_VIEW: return ((struct nvn_gpu_view_s *)obj)->released;
    case REF_SAMP: return ((struct nvn_gpu_samp_s *)obj)->released;
    default: return 1;
    }
}

static void ref_bump(void *obj, int type) {
    switch (type) {
    case REF_BUF: ((struct nvn_gpu_buf_s *)obj)->refs++; break;
    case REF_TEX: ((struct nvn_gpu_tex_s *)obj)->refs++; break;
    case REF_VIEW: ((struct nvn_gpu_view_s *)obj)->refs++; break;
    case REF_SAMP: ((struct nvn_gpu_samp_s *)obj)->refs++; break;
    }
}

static void ref_drop(void *obj, int type) {
    switch (type) {
    case REF_BUF: if (((struct nvn_gpu_buf_s *)obj)->refs > 0) ((struct nvn_gpu_buf_s *)obj)->refs--; break;
    case REF_TEX: if (((struct nvn_gpu_tex_s *)obj)->refs > 0) ((struct nvn_gpu_tex_s *)obj)->refs--; break;
    case REF_VIEW: if (((struct nvn_gpu_view_s *)obj)->refs > 0) ((struct nvn_gpu_view_s *)obj)->refs--; break;
    case REF_SAMP: if (((struct nvn_gpu_samp_s *)obj)->refs > 0) ((struct nvn_gpu_samp_s *)obj)->refs--; break;
    }
}

// 录制点（函数名 + 行号）随报错带出：「release 之后仍在使用」只在真机长局里出现（#937），
// 光凭一句话分不清是哪类句柄、哪条录制路径，普查报告里这一行就要能定位。
#define cmd_ref(c, obj, type) cmd_ref_at((c), (obj), (type), __func__, __LINE__)
static void cmd_ref_at(struct nvn_gpu_cmd_s *c, void *obj, int type, const char *fn, int line) {
    if (!obj) return;
    pthread_mutex_lock(&g_mu);
    if (ref_released(obj, type)) {   // release 之后再录制使用 = 契约违规（粘滞 E_ARG）
        pthread_mutex_unlock(&g_mu);
        static const char *const names[] = {"?", "buf", "tex", "view", "samp"};
        set_sticky(c, NVN_GPU_E_ARG, "release 之后仍在使用句柄（%s %p，录制于 %s:%d）",
                   type >= REF_BUF && type <= REF_SAMP ? names[type - REF_BUF + 1] : names[0], obj, fn, line);
        return;
    }
    ref_bump(obj, type);
    pthread_mutex_unlock(&g_mu);
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {   // 记录失败视为 OOM：整段不提交；刚加的引用要当场归还
            pthread_mutex_lock(&g_mu);
            ref_drop(obj, type);
            pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return;
        }
        c->refs = p;
        c->refcap = cap;
    }
    c->refs[c->nrefs].obj = obj;
    c->refs[c->nrefs].type = type;
    c->nrefs++;
}

// 调用方持 g_mu。
static void unref_locked(struct nvn_gpu_vk *vk, void *obj, int type) {
    if (!obj) return;
    if (type == REF_BUF) {
        struct nvn_gpu_buf_s *b = obj;
        if (b->refs > 0) b->refs--;
        if (b->released && b->refs == 0) queue_retire_locked(vk, b);
        return;
    }
    if (type == REF_TEX) {
        struct nvn_gpu_tex_s *t = obj;
        if (t->refs > 0) t->refs--;
        if (t->released && t->refs == 0) destroy_tex_locked(vk, t);
        return;
    }
    if (type == REF_VIEW) {
        struct nvn_gpu_view_s *v = obj;
        if (v->refs > 0) v->refs--;
        if (v->released && v->refs == 0) destroy_view_locked(vk, v);
        return;
    }
    if (type == REF_SAMP) {
        struct nvn_gpu_samp_s *s = obj;
        if (s->refs > 0) s->refs--;
        if (s->released && s->refs == 0) destroy_samp_locked(vk, s);
    }
}

static void unlink_buf_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    struct nvn_gpu_buf_s **pp = &vk->bufs;
    while (*pp && *pp != b) pp = &(*pp)->all_next;
    if (*pp) *pp = b->all_next;
    b->all_next = NULL;
}

static void retire_one(struct nvn_gpu_vk *vk, struct nvn_gpu_buf_s *b) {
    nvn_gpu_retired_fn cb = b->retired;
    void *user = b->retired_user;
    pthread_mutex_lock(&g_mu);
    unlink_buf_locked(vk, b);
    pthread_mutex_unlock(&g_mu);
    if (b->mapped && vk->d) vkUnmapMemory(vk->d, b->memory);
    if (b->buffer) vkDestroyBuffer(vk->d, b->buffer, NULL);
    if (b->memory) vkFreeMemory(vk->d, b->memory, NULL);
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->mapped = NULL;
    free(b);
    pthread_mutex_lock(&g_mu);
    vk->stats[STAT_RETIRED]++;
    pthread_mutex_unlock(&g_mu);
    if (cb) {
        g_in_callback++;
        cb(user);
        g_in_callback--;
    }
}

// ---------------------------------------------------------------- 设备 / 能力

static uint32_t sample_bits_max(VkSampleCountFlags f) {
    uint32_t n = 0;
    for (uint32_t s = 1; s <= 64; s <<= 1)
        if (f & s) n = s;
    return n;
}

static void caps_digest(const void *p, size_t n, uint8_t out[16]) {
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h1 = (h1 ^ b[i]) * 1099511628211ull;
        h2 = (h2 + b[i]) * 1469598103934665603ull;
    }
    memcpy(out, &h1, 8);
    memcpy(out + 8, &h2, 8);
}

static void fill_caps(struct nvn_gpu_vk *vk, const nvn_gpu_open_desc *d) {
    const VkPhysicalDeviceProperties *p = &vk->dev->properties;
    const VkPhysicalDeviceLimits *l = &p->limits;
    nvn_gpu_caps *c = &vk->caps;
    memset(c, 0, sizeof *c);
    c->api = NVN_GPU_API_VULKAN;
    c->api_version = p->apiVersion;
    c->family = 0;
    c->vendor_id = p->vendorID;
    c->device_id = p->deviceID;
    snprintf(c->name, sizeof c->name, "%.63s", p->deviceName);
    c->max_color_att = l->maxColorAttachments;
    c->max_vertex_streams = l->maxVertexInputBindings;
    c->max_tex_dim = l->maxImageDimension2D;
    c->max_samples = sample_bits_max(l->framebufferColorSampleCounts & l->framebufferDepthSampleCounts);
    c->max_viewports = l->maxViewports;
    c->max_push_bytes = l->maxPushConstantsSize;
    c->min_ubo_align = (uint32_t)l->minUniformBufferOffsetAlignment;
    c->min_ssbo_align = (uint32_t)l->minStorageBufferOffsetAlignment;
    c->max_ubo_range = l->maxUniformBufferRange;
    c->max_ssbo_range = l->maxStorageBufferRange;   // 描述符「范围」上限（不等于单次分配上限）
    // 单次分配上限来自 VkPhysicalDeviceMaintenance3Properties.maxMemoryAllocationSize（Vulkan 1.1）；
    // 以前这里错用了 maxStorageBufferRange，于是任何大于 128 MiB 的池镜像都建不出来。
    { VkPhysicalDeviceMaintenance3Properties m3; memset(&m3, 0, sizeof m3); m3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2); p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &m3;
      vkGetPhysicalDeviceProperties2(vk->phys, &p2);
      c->max_buf_bytes = m3.maxMemoryAllocationSize ? m3.maxMemoryAllocationSize : (uint64_t)l->maxStorageBufferRange; }
    c->max_stage_res[NVN_GPU_RES_UBO] = l->maxPerStageDescriptorUniformBuffers;
    c->max_stage_res[NVN_GPU_RES_SSBO] = l->maxPerStageDescriptorStorageBuffers;
    c->max_stage_res[NVN_GPU_RES_TEX] = l->maxPerStageDescriptorSampledImages;
    c->max_stage_res[NVN_GPU_RES_SAMP] = l->maxPerStageDescriptorSamplers;
    uint32_t comb = l->maxPerStageDescriptorSampledImages < l->maxPerStageDescriptorSamplers
                        ? l->maxPerStageDescriptorSampledImages : l->maxPerStageDescriptorSamplers;
    c->max_stage_res[NVN_GPU_RES_TEX_SAMP] = comb;
    c->max_stage_res[NVN_GPU_RES_IMAGE] = l->maxPerStageDescriptorStorageImages;
    c->max_sets = l->maxBoundDescriptorSets;

    VkPhysicalDeviceFeatures f = vk->dev->core_features;   // 已启用记录，不是「设备支持」
    // debug 强制设备特性不能绕开 GS+遮挡探测门；缺记录或未通过保持未接。
    const char *gs_probe = s2a_capprobe_gs_occ_state ? s2a_capprobe_gs_occ_state() : NULL;
    const char *gs_verdict = gs_probe ? strrchr(gs_probe, ':') : NULL;
    int gs_enabled = f.geometryShader && gs_verdict && !strcmp(gs_verdict + 1, "supported");
    if (gs_enabled) c->caps |= NVN_GPU_CAP_GEOMETRY;
    if (gs_enabled && f.vertexPipelineStoresAndAtomics) {
        c->storage_stages |= 1u << NVN_GPU_SH_GS;
        c->atomic_stages |= 1u << NVN_GPU_SH_GS;
    }
    if (f.vertexPipelineStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->storage_stages |= 1u << NVN_GPU_SH_FS;
    c->storage_stages |= 1u << NVN_GPU_SH_CS;
    if (f.vertexPipelineStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_VS;
    if (f.fragmentStoresAndAtomics) c->atomic_stages |= 1u << NVN_GPU_SH_FS;
    c->atomic_stages |= 1u << NVN_GPU_SH_CS;

    // #1199：BC 不再按 textureCompressionBC 一刀切——原生全有或缺的格式能走 compute 解码就算支持。
    if (f.textureCompressionBC || vk->dev->bc_decode_mask) c->caps |= NVN_GPU_CAP_TEX_BC;
    if (f.textureCompressionASTC_LDR) c->caps |= NVN_GPU_CAP_TEX_ASTC;
    c->caps |= NVN_GPU_CAP_BLIT_SCALED;   // vkCmdBlitImage；具体格式对行不行在 nvn_gpu_blit 里逐次按格式特性判
    // B2：设备 advertise VK_GOOGLE_display_timing 且 debug.s2a.display_timing=1（默认关）才有
    // 每帧实际上屏时刻可报——能力位如实反映 advertise∧启用，时刻取不到时按契约报 UNKNOWN。
    if (vk->dev->has_display_timing) c->caps |= NVN_GPU_CAP_SHOWN_TIME;
    if (vk->dev->draw_indirect_count_enabled && vk->dev->draw_indirect_count &&
        vk->dev->draw_indexed_indirect_count && f.multiDrawIndirect && f.drawIndirectFirstInstance)
        c->caps |= NVN_GPU_CAP_INDIRECT_COUNT;
    // HOST_IMPORT / GPU_VA / TILE_MEMORY / SAMP_MINMAX / LOD_BIAS / DEPTH_CLIP_CTL / GEOMETRY / TESSELLATION /
    // XFB / LINKED_PIPE / GPU_DURATION / GPU_TIME_HOST / DYN_* / DESWIZZLE：
    // 本段没有实现或没实测生效，一律不置位（契约：后端不做近似）。

    if (d->caps_record && d->caps_record_bytes) {
        caps_digest(d->caps_record, d->caps_record_bytes, c->caps_id);
    } else {
        nvn_gpu_caps tmp = *c;   // 摘要覆盖整份能力记录（含限值与能力位），caps_id 自己清零
        memset(tmp.caps_id, 0, sizeof tmp.caps_id);
        caps_digest(&tmp, sizeof tmp, c->caps_id);
    }
}

// ---------------------------------------------------------------- worker

static void segment_done(struct vk_segment *s, int status, int code, const char *why) {
    if (!s->done) return;
    nvn_gpu_done d;
    memset(&d, 0, sizeof d);
    d.signal = s->signal;
    d.status = (uint32_t)status;
    d.rc = rc(code);
    d.why = why;
    d.time_kind = NVN_GPU_TIME_NONE;
    d.occlusion = NULL;
    d.nocclusion = 0;
    // 遮挡结果：段已完成，用过的槽都有结果；没用过的槽读作 0（不等待，取不到的项保持预置的 0）
    uint64_t *occ = NULL;
    if (status == NVN_GPU_DONE_OK && s->cmd && s->cmd->qpool && s->cmd->qmax_used && (occ = calloc(s->cmd->qcount, sizeof *occ))) {
        for (uint32_t i = 0; i < s->cmd->qmax_used; i++)
            if (s->cmd->q_used[i]) (void)vkGetQueryPoolResults(g.d, s->cmd->qpool, i, 1, sizeof *occ, &occ[i], sizeof *occ, VK_QUERY_RESULT_64_BIT);
        d.occlusion = occ; d.nocclusion = s->cmd->qcount;
    }
    g_in_callback++;
    s->done(s->user, &d);
    g_in_callback--;
    free(occ);
}

static void detach_live(struct nvn_gpu_cmd_s *c) {
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) *pp = c->live_next;
    c->live_next = NULL;
    pthread_mutex_unlock(&g_mu);
}

static void finish_cmd(struct nvn_gpu_cmd_s *c, int executed) {
    if (!c) return;
    // 布局按段跟踪（c->lay）：段不执行时命令缓冲里什么都没跑，没有要回滚的；t->layout 只做诊断。
    detach_live(c);   // 已提交的 cmd 在 commit 时已摘下，这里兜底
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < c->n_newimg; i++) {   // 段没执行：图像还是 UNDEFINED，放回待转表（放不回就留给下面的归还：没人再用它）
        if (!executed && !c->newimg[i]->released) newimg_add_locked(c->newimg[i]);
        unref_locked(&g, c->newimg[i], REF_TEX);
    }
    free(c->newimg); c->newimg = NULL; c->n_newimg = 0;
    for (uint32_t i = 0; i < c->nrefs; i++) unref_locked(&g, c->refs[i].obj, c->refs[i].type);
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < c->nstaging; i++) {
        struct nvn_gpu_buf_s *b = c->staging[i].buf;
        if (!b) continue;
        if (seg_give_stg(b)) continue;
        pthread_mutex_lock(&g_mu);
        unlink_buf_locked(&g, b);
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    for (uint32_t i = 0; i < c->npasses; i++) {
        if (c->passes[i].fb) vkDestroyFramebuffer(g.d, c->passes[i].fb, NULL);
        if (c->passes[i].rp) vkDestroyRenderPass(g.d, c->passes[i].rp, NULL);
    }
    free(c->passes);
    free(c->lay);
    free(c->memo);
    for (int i = 0; i < 6; i++) free(c->push_data[i]);
    seg_give_desc(c->dpool);
    for (uint32_t i = 0; i < c->ndpools_full; i++) seg_give_desc(c->dpools_full[i]);
    free(c->dpools_full);
    if (c->tsq) vkDestroyQueryPool(g.d, c->tsq, NULL);
        seg_give_q(c->qpool, c->qcount);
    free(c->q_used);
    while (c->bc_temps) {
        struct bc_temp *b = c->bc_temps; c->bc_temps = b->next;
        nvn_vk_bcdec_rec_free(g.dev, &b->rec);
        if (b->image) vkDestroyImage(g.d, b->image, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    seg_give_cmd(c->pool, c->cb);
    free(c->refs);
    free(c->staging);
    free(c);
}

static void process_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    int failed = s->failed;
    nvn_gpu_rc code = s->rc;
    const char *why = s->why[0] ? s->why : "段未执行";

    if (!failed) {
        pthread_mutex_lock(&g_mu);
        uint64_t fail_sig = g.test_fail_signal, delay_sig = g.test_delay_signal;
        uint32_t delay_ms = g.test_delay_ms;
        pthread_mutex_unlock(&g_mu);
        if (delay_ms && delay_sig == s->signal) {
            struct timespec ts = {(time_t)(delay_ms / 1000), (long)(delay_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        VkResult r = VK_TIMEOUT;
        while (!vk->stop && !vk->device_lost) {
            r = vkWaitForFences(vk->d, 1, &s->fence, VK_TRUE, 50ull * 1000ull * 1000ull);
            if (r != VK_TIMEOUT) break;
        }
        // 测试钩子：真实栅栏必须先完成（否则 fence / command buffer 还在途，不能销毁），
        // 完成后再把它当成失败，走真实的失败终止路径。
        if (r == VK_SUCCESS && fail_sig && fail_sig == s->signal) r = (VkResult)VK_ERROR_UNKNOWN;
        if (r != VK_SUCCESS) {
            pthread_mutex_lock(&g_mu);
            if (r == VK_ERROR_DEVICE_LOST) vk->device_lost = 1;
            if (!s->internal && (!vk->failed || s->signal < vk->failed)) vk->failed = s->signal;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
            // #1456：真实 DEVICE_LOST 回写当前指纹 capprobe 结果（source=runtime-lost，
            // 只记指纹不记游戏），下次启动直接走规避不再探；JNI 回呼不持 g_mu。
            if (r == VK_ERROR_DEVICE_LOST && s2a_capprobe_runtime_lost) s2a_capprobe_runtime_lost();
            failed = 1;
            code = rc(r == VK_ERROR_DEVICE_LOST ? NVN_GPU_E_DEVICE : NVN_GPU_E_FAILED);
            why = r == VK_ERROR_DEVICE_LOST ? "设备丢失" : "等待栅栏失败";
        } else {
            if (s->cmd && s->cmd->tsq && !s->internal) {   // 调试读数：这一段在 GPU 上首尾相隔多久；相邻两段之间 GPU 空了多久
                static double period; static uint64_t n, sum, mx, last_end, gap, t_first;
                if (period == 0) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g.phys, &pp); period = pp.limits.timestampPeriod > 0 ? pp.limits.timestampPeriod : 1; }
                uint64_t ts[2] = {0, 0};
                if (vkGetQueryPoolResults(vk->d, s->cmd->tsq, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
                    uint64_t d = (uint64_t)((double)(ts[1] - ts[0]) * period), b = (uint64_t)((double)ts[0] * period), e = (uint64_t)((double)ts[1] * period);
                    if (!n) t_first = b;
                    if (last_end && b > last_end) gap += b - last_end;
                    last_end = e; n++; sum += d; if (d > mx) mx = d;
                    {   // 逐通道：按尺寸归拢（同尺寸的算一类），每 400 段报耗时最多的 6 类
                        static struct { uint32_t key, n, draws, ds, n1; uint64_t ns, ns1; } bk[48]; static int nbk;
                        static uint64_t pts[2 * TS_PASS_MAX];
                        uint32_t np = s->cmd->ntsp;
                        if (np && vkGetQueryPoolResults(vk->d, s->cmd->tsq, 2, 2 * np, sizeof pts[0] * 2 * np, pts, sizeof pts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                            for (uint32_t i = 0; i < np; i++) { uint32_t key = (uint32_t)s->cmd->tsp[i].w << 16 | s->cmd->tsp[i].h; int k = 0;
                                while (k < nbk && bk[k].key != key) k++;
                                if (k == nbk) { if (nbk == 48) continue; bk[nbk++].key = key; bk[k].n = bk[k].draws = bk[k].ds = bk[k].n1 = 0; bk[k].ns = bk[k].ns1 = 0; }
                                bk[k].n++; bk[k].draws += s->cmd->tsp[i].draws; bk[k].ds += s->cmd->tsp[i].ds;
                                uint64_t dns = pts[2 * i + 1] >= pts[2 * i] ? (uint64_t)((double)(pts[2 * i + 1] - pts[2 * i]) * period) : 0;
                                bk[k].ns += dns;
                                if (s->cmd->tsp[i].draws <= 1) { bk[k].n1++; bk[k].ns1 += dns; }   // 0–1 笔通道：单独累计，看它们到底吃掉多少 GPU 时间
                            }
                        if (n == 400) { char l2[700]; size_t o = 0; l2[0] = 0;
                            for (int r = 0; r < 6 && r < nbk; r++) { int best = r; for (int k = r + 1; k < nbk; k++) if (bk[k].ns > bk[best].ns) best = k;
                                if (best != r) { __typeof__(bk[0]) t = bk[r]; bk[r] = bk[best]; bk[best] = t; }
                                o += (size_t)snprintf(l2 + o, sizeof l2 - o, "%s%ux%u：%u 个（带深度 %u、0–1 笔 %u 个耗 %.0f）%u 笔 %.0f 毫秒", r ? "；" : "", bk[r].key >> 16, bk[r].key & 0xffff, bk[r].n, bk[r].ds, bk[r].n1, (double)bk[r].ns1 / 1e6, bk[r].draws, (double)bk[r].ns / 1e6); if (o >= sizeof l2) break; }
                            char l3[800]; snprintf(l3, sizeof l3, "GPU 时间（按通道尺寸，同样这 400 段）：%s", l2); nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", l3); nbk = 0; }
                    }
                    if (n == 400) { char line[300]; snprintf(line, sizeof line, "GPU 时间：%llu 段，段内首尾平均 %.2f 毫秒、最长 %.2f 毫秒；这些段前后共 %.0f 毫秒，其中 GPU 在段内 %.0f 毫秒、段与段之间空着 %.0f 毫秒",
                                                         (unsigned long long)n, (double)sum / 1e6 / (double)n, (double)mx / 1e6, (double)(e - t_first) / 1e6, (double)sum / 1e6, (double)gap / 1e6);
                                    nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line); n = 0; sum = mx = gap = 0; }
                }
            }
            // 前面的段已经把时间线打进失败终态时，本段即使执行完也不许发布成功（A4）。
            pthread_mutex_lock(&g_mu);
            int terminal = vk->failed && s->signal >= vk->failed;
            pthread_mutex_unlock(&g_mu);
            if (terminal) {
                failed = 1;
                code = rc(NVN_GPU_E_FAILED);
                why = "时间线已失败，本段不发布成功";
            }
        }
    }

    if (s->internal) {   // 呈现段：没有回调、不动时间线
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    } else if (failed) {
        segment_done(s, NVN_GPU_DONE_FAILED, code.code, why);
        pthread_mutex_lock(&g_mu);
        vk->stats[STAT_DONE_FAILED]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    } else {
        // 发布顺序：先 done（核心回写），done 返回后才推进已完成值。
        segment_done(s, NVN_GPU_DONE_OK, NVN_GPU_OK, NULL);
        pthread_mutex_lock(&g_mu);
        if (s->signal > 0 && s->signal > vk->completed) vk->completed = s->signal;
        submitted_compact_locked(vk);
        vk->stats[STAT_DONE_OK]++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (s->fence) vkDestroyFence(vk->d, s->fence, NULL);
    finish_cmd(s->cmd, s->executed);
    free(s);
}

static void *worker_main(void *arg) {
    struct nvn_gpu_vk *vk = arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (vk->rhead) {
            struct nvn_gpu_buf_s *b = vk->rhead;
            vk->rhead = b->retire_next;
            if (!vk->rhead) vk->rtail = NULL;
            b->retire_next = NULL;
            pthread_mutex_unlock(&g_mu);
            retire_one(vk, b);
            pthread_mutex_lock(&g_mu);
            continue;
        }
        if (vk->qhead) {
            struct vk_segment *s = vk->qhead;
            vk->qhead = s->next;
            if (!vk->qhead) vk->qtail = NULL;
            vk->busy = 1;
            pthread_mutex_unlock(&g_mu);
            process_segment(vk, s);
            pthread_mutex_lock(&g_mu);
            vk->busy = 0;
            pthread_cond_broadcast(&g_cv);
            continue;
        }
        if (vk->stop) break;
        struct timespec ts;
        deadline_after(&ts, VK_WAIT_SLICE_NS);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

// ---------------------------------------------------------------- 驱动管线缓存（#822，见 g_plc 一节的总说明）

static void plc_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void plc_logf(int level, const char *fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nvn_plat_log(level, "gpu", b);
}

static double plc_ms(uint64_t from, uint64_t to) { return (double)(to - from) / 1e6; }

// 缓存目录还不存在就补上（最多再补一层父目录，和程序缓存 direct/program_cache.c 的 ensure_dir 同口径：目录通常是
// <状态目录>/direct-cache/nvn-vk，第一局可能谁都还没建过）。已存在 / 建不出都不在这里报错：写盘那一步会如实记下原因。
static void plc_ensure_dir(const char *dir) {
    if (mkdir(dir, 0700) == 0 || errno != ENOENT) return;
    char parent[sizeof g_plc_dir];
    snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return;
    *slash = 0;
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) return;
    (void)mkdir(dir, 0700);
}

// 开会话时：读旧文件（有就喂给驱动）→ 建缓存。线程：与 open 同。任何失败都落到「没有缓存」，不让 open 失败。
static void plc_open(const char *dir) {
    g_plc = VK_NULL_HANDLE;
    g_plc_dir[0] = 0;
    g_plc_loaded = 0;
    g_plc_rejected = 0;
    g_plc_made = 0;
    g_plc_t_open = now_ns();
    __atomic_store_n(&g_plc_t_saved, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plc_made_saved, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plcw_failed, 0, __ATOMIC_RELAXED);
    g_plc_size_saved = 0;
    memcpy(g_plc_uuid, g.dev->properties.pipelineCacheUUID, sizeof g_plc_uuid);
    if (!nvn_plat_flag("nvk_pipe_cache", 1)) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：开关 nvk_pipe_cache=0，管线不带缓存建");
        return;
    }
    {   // 局中补写的阈值（缺省值见 g_plc_made 一节；条数至少 1，毫秒不取负）
        int n = nvn_plat_flag("nvk_plc_new", 64), f = nvn_plat_flag("nvk_plc_first_ms", 10000), e = nvn_plat_flag("nvk_plc_every_ms", 30000);
        g_plc_thr_new = n < 1 ? 1u : (uint64_t)n;
        g_plc_first_ns = (uint64_t)(f < 0 ? 0 : f) * 1000000ull;
        g_plc_every_ns = (uint64_t)(e < 0 ? 0 : e) * 1000000ull;
    }
    void *blob = NULL;
    size_t bytes = 0;
    if (!dir || !dir[0]) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：平台没给缓存目录，只在本局内复用，不落盘");
    } else if (strlen(dir) >= sizeof g_plc_dir) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：缓存目录路径太长，只在本局内复用，不落盘");
    } else {
        snprintf(g_plc_dir, sizeof g_plc_dir, "%s", dir);
        char why[96] = "";
        uint64_t t0 = now_ns();
        int r = nvk_driver_cache_read(g_plc_dir, g_plc_uuid, &blob, &bytes, why, sizeof why);
        if (r < 0) {   // 损坏 / 版本不符 / 超限：当没有，本局收尾会换成新的
            g_plc_rejected++;
            blob = NULL;
            bytes = 0;
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：旧文件不能用（%s），当作没有，本局收尾换成新的", why);
        } else if (r > 0) {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：目录里还没有旧文件（头一局，或被清过）");
        } else {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：读到旧文件 %zu 字节（%.1f ms）", bytes, plc_ms(t0, now_ns()));
        }
    }
    VkPipelineCacheCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = bytes;
    ci.pInitialData = bytes ? blob : NULL;
    VkResult vr = vkCreatePipelineCache(g.d, &ci, NULL, &g_plc);
    if (vr != VK_SUCCESS && bytes) {   // 驱动不收这份旧数据：丢掉它、用空缓存重建，别让旧文件挡住缓存
        g_plc_rejected++;
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：驱动不收旧数据（VkResult %d），改用空缓存", (int)vr);
        bytes = 0;
        g_plc = VK_NULL_HANDLE;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vr = vkCreatePipelineCache(g.d, &ci, NULL, &g_plc);
    }
    free(blob);
    if (vr != VK_SUCCESS) {
        g_plc = VK_NULL_HANDLE;
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：驱动建不出缓存（VkResult %d），管线照旧不带缓存", (int)vr);
        return;
    }
    g_plc_loaded = bytes;
    g_plc_size_saved = bytes;   // 盘上现有的、驱动也收下了的数据大小；驱动不收旧数据时 bytes 已清零 → 取到的任何数据都比它「新」，收尾会换掉旧文件
}

// ---- 局中补写（#868）：后台写盘线程 + 触发 ----

// 交给后台线程的一份活：数据缓冲区、落盘目录、缓存标识都是自己的副本——后台线程不碰任何 Vulkan 句柄，也不碰会话全局。
struct plcw_job {
    void *data;
    size_t size;
    char dir[sizeof g_plc_dir];
    uint8_t uuid[VK_UUID_SIZE];
    unsigned long long made;   // 触发时本局经缓存建管线的条数（只用于日志）
    double get_ms;             // 触发线程上花的毫秒：查大小 + 取数据 + 拷字节（只用于日志）
};

// 后台写盘线程（分离线程）：写一份、收尾、清「在飞」标记（最后一步，收尾那边靠它判断写完没有）。写失败只记日志，收尾时会再试一次。
static void *plcw_main(void *arg) {
    struct plcw_job *j = (struct plcw_job *)arg;
    char why[96] = "";
    uint64_t t0 = now_ns();
    plc_ensure_dir(j->dir);
    int w = nvk_driver_cache_write(j->dir, j->uuid, j->data, j->size, why, sizeof why);
    double ms = plc_ms(t0, now_ns());
    if (w == 0) {
        __atomic_store_n(&g_plcw_failed, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_plcw_ok, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中写出 %zu 字节（本局经缓存建管线 %llu 条；触发线程用时 %.1f ms，后台写盘 %.1f ms）", j->size, j->made, j->get_ms, ms);
    } else {
        __atomic_store_n(&g_plcw_failed, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_plcw_fail, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中写盘没写成（%s）。收尾时会再试一次，本局不受影响", why);
    }
    free(j->data);
    free(j);
    __atomic_store_n(&g_plcw_busy, 0, __ATOMIC_RELEASE);
    return NULL;
}

// 触发检查已过（条数够了、时间到了）：在这条线程上取一份驱动数据，交给后台线程写。线程：任意建管线的线程（可能还持着清屏管线的锁 g_clr_mu）。
// 拿不到 g_plc_mu（别的线程正在取数据，或收尾在销毁缓存）/ 上一次写还在飞 → 直接跳过，下次建管线再碰，不排队、不等。
// force = 1：不看条数 / 时间阈值（停局保留会话时用，见 nvn_gpu_vk_pipe_cache_flush），其余照旧：数据没变不写、写在飞就跳过。
static void plc_save_midrun_impl(int force) {
    if (__atomic_load_n(&g_plcw_busy, __ATOMIC_ACQUIRE)) return;
    if (pthread_mutex_trylock(&g_plc_mu) != 0) return;
    VkPipelineCache pc = g_plc;
    uint64_t t = now_ns(), made = __atomic_load_n(&g_plc_made, __ATOMIC_RELAXED), ts = __atomic_load_n(&g_plc_t_saved, __ATOMIC_RELAXED);
    int due = pc && g_plc_dir[0] && !g.device_lost &&
              (force || (made >= __atomic_load_n(&g_plc_made_saved, __ATOMIC_RELAXED) + g_plc_thr_new &&
                         (ts ? t - ts >= g_plc_every_ns : t - g_plc_t_open >= g_plc_first_ns)));
    int claimed = 0;   // 抢到「在飞」标记 = 这一刻没有别的写，也轮到本线程来写
    if (due) {
        int expect = 0;
        claimed = __atomic_compare_exchange_n(&g_plcw_busy, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
    if (!claimed) { pthread_mutex_unlock(&g_plc_mu); return; }
    // 到点了。基线先推进到现在：这次取不到数据 / 数据没变 / 起不了线程，都要等再多 N 条、再隔 E 才重试，别每建一条管线就来一遍。
    __atomic_store_n(&g_plc_made_saved, made, __ATOMIC_RELAXED);
    __atomic_store_n(&g_plc_t_saved, t, __ATOMIC_RELAXED);
    uint64_t tg0 = now_ns();   // 触发线程在这里花的时间从这儿算起：查大小 + 取数据 + 拷字节（带 fsync 的写盘不在这条线程上）
    size_t size = 0;
    VkResult vr = vkGetPipelineCacheData(g.d, pc, &size, NULL);
    void *buf = NULL;
    int started = 0;   // 后台线程已接手（之后「在飞」标记由它清）；没接手的路径在函数末尾自己清
    if (vr != VK_SUCCESS || size == 0) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中取不到驱动数据（VkResult %d，%zu 字节），这次不落盘", (int)vr, size);
    } else if (size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&g_plcw_same, 1, __ATOMIC_RELAXED);
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中检查：本局经缓存建管线 %llu 条，数据 %zu 字节与上次保存一致，不写", (unsigned long long)made, size);
    } else {
        size_t cap = size + size / 4 + 4096;   // 留余量：取大小之后别的线程又建了管线，数据可能已经变大（VK_INCOMPLETE 的半份数据不能落盘）
        buf = malloc(cap);
        if (!buf) {
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：内存不够取数据（%zu 字节），这次不落盘", cap);
        } else {
            size_t got = cap;
            vr = vkGetPipelineCacheData(g.d, pc, &got, buf);
            if (vr != VK_SUCCESS || got == 0 || got > cap) {
                plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：局中取数据失败（VkResult %d），这次不落盘", (int)vr);
                free(buf);
                buf = NULL;
            } else if (got == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
                // 查询给的大小可以是上界（lavapipe 就比实际取到的大），上面按查询大小比不出「没变」：按实际取到的字节数再比一次
                __atomic_fetch_add(&g_plcw_same, 1, __ATOMIC_RELAXED);
                plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：局中检查：本局经缓存建管线 %llu 条，数据 %zu 字节与上次保存一致，不写", (unsigned long long)made, got);
            } else {
                size = got;
                struct plcw_job *j = (struct plcw_job *)calloc(1, sizeof *j);
                if (j) {
                    j->data = buf; j->size = size; j->made = (unsigned long long)made; j->get_ms = plc_ms(tg0, now_ns());
                    memcpy(j->dir, g_plc_dir, sizeof j->dir);
                    memcpy(j->uuid, g_plc_uuid, sizeof j->uuid);
                    pthread_attr_t at;
                    pthread_t th;
                    if (pthread_attr_init(&at) == 0) {
                        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                        pthread_attr_setstacksize(&at, 256 * 1024);
                        __atomic_fetch_add(&g_plcw_started, 1, __ATOMIC_RELAXED);   // 先记再起：后台线程一旦跑完，读数里「成功 + 失败」不会比「次数」多
                        started = pthread_create(&th, &at, plcw_main, j) == 0;
                        if (!started) __atomic_fetch_sub(&g_plcw_started, 1, __ATOMIC_RELAXED);
                        pthread_attr_destroy(&at);
                    }
                    if (started) {
                        g_plc_size_saved = size;
                        buf = NULL;   // 缓冲区和 j 都归后台线程了
                    } else {
                        free(j);
                    }
                }
                if (!started) plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：起不了后台写盘线程，这次不落盘");
            }
        }
    }
    free(buf);
    if (!started) __atomic_store_n(&g_plcw_busy, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_plc_mu);
}

static void plc_save_midrun(void) { plc_save_midrun_impl(0); }

// 停局但会话保留到进程退出（#860：前端没停稳就不关后端，也就走不到 plc_close 的收尾写）：把驱动当前缓存取一份交后台线程写盘，
// 不销毁缓存、不等写完——写盘是临时文件 + 原子替换，进程中途退出旧文件原样。调用方线程只花取数据的时间（真机约 0.5 ms）。线程：任意。
void nvn_gpu_vk_pipe_cache_flush(void) {
    if (!g_open) return;
    plc_save_midrun_impl(1);
}

// 一条管线刚经缓存建好：记账；够触发局中补写的条数、时间也到了就试一次。线程：任意建管线的线程（图形 / 计算 / 清屏）。
static void plc_created(void) {
    if (!g_plc) return;
    uint64_t made = __atomic_add_fetch(&g_plc_made, 1, __ATOMIC_RELAXED);
    if (!g_plc_dir[0] || made < __atomic_load_n(&g_plc_made_saved, __ATOMIC_RELAXED) + g_plc_thr_new) return;
    uint64_t ts = __atomic_load_n(&g_plc_t_saved, __ATOMIC_RELAXED), t = now_ns();
    if (ts ? t - ts < g_plc_every_ns : t - g_plc_t_open < g_plc_first_ns) return;
    plc_save_midrun();
}

// 等在飞的后台写收尾，最多等到 deadline（now_ns 时钟）。返回 1 = 已经没有在飞的写，0 = 到点了还没写完。
static int plcw_wait(uint64_t deadline) {
    while (__atomic_load_n(&g_plcw_busy, __ATOMIC_ACQUIRE)) {
        if (now_ns() >= deadline) return 0;
        struct timespec ts = {0, 1000000};   // 1 ms 一查：写盘通常十几毫秒，不值得为它再配一套条件变量
        nanosleep(&ts, NULL);
    }
    return 1;
}

// 收尾时（所有段都完成之后、销毁管线之前）：把驱动当前的缓存数据写盘，再销毁缓存。线程：与 close 同。
// 先等在飞的局中写（等多久算进 close 的 deadline；到点没写完 = 放弃这次收尾写：两边写同一个临时文件，不能并行，也不拖 close）。
// 数据大小与上次保存 / 读入时一致就不重写；设备丢了不取数据；写失败只记日志（下一局照旧慢一次，本局不受影响）。
static void plc_close(uint64_t deadline) {
    pthread_mutex_lock(&g_plc_mu);   // 与局中触发互斥：拿到之后不会再有线程拿 g_plc 去取数据
    VkPipelineCache pc = g_plc;
    g_plc = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_plc_mu);
    if (!pc) return;
    unsigned long long made = (unsigned long long)__atomic_load_n(&g_plc_made, __ATOMIC_RELAXED);
    if (!plcw_wait(deadline)) {
        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：close 时限内局中写盘还没写完，放弃收尾这次写（旧文件原样，下一局照常读）");
    } else if (g_plc_dir[0] && g.device_lost) {
        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：设备已丢失，不取数据、不落盘");
    } else if (g_plc_dir[0]) {
        uint64_t t0 = now_ns();
        size_t size = 0;
        VkResult vr = vkGetPipelineCacheData(g.d, pc, &size, NULL);
        if (vr != VK_SUCCESS || size == 0) {
            plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：取不到驱动数据（VkResult %d，%zu 字节），不落盘", (int)vr, size);
        } else if (size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
            plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：本局没有新增（%zu 字节，与上次保存 / 读入时一致），不重写；本局经缓存建管线 %llu 条", size, made);
        } else {
            void *buf = malloc(size);
            if (!buf) {
                plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：内存不够取数据（%zu 字节），不落盘", size);
            } else {
                size_t want = size;
                vr = vkGetPipelineCacheData(g.d, pc, &size, buf);
                uint64_t t1 = now_ns();
                if (vr == VK_SUCCESS && size > 0 && size <= want && size == g_plc_size_saved && !__atomic_load_n(&g_plcw_failed, __ATOMIC_ACQUIRE)) {
                    // 查询大小可以是上界（同局中补写）：按实际取到的字节数再判一次「没变」
                    plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：本局没有新增（取到 %zu 字节，与上次保存 / 读入时一致），不重写；本局经缓存建管线 %llu 条", size, made);
                } else if (vr == VK_SUCCESS && size > 0 && size <= want) {
                    char why[96] = "";
                    plc_ensure_dir(g_plc_dir);
                    int w = nvk_driver_cache_write(g_plc_dir, g_plc_uuid, buf, size, why, sizeof why);
                    uint64_t t2 = now_ns();
                    if (w == 0)
                        plc_logf(NVN_PLAT_LOG_INFO, "管线缓存：写出 %zu 字节（读入时 %zu；本局经缓存建管线 %llu 条；取数据 %.1f ms，写盘 %.1f ms）",
                                 size, g_plc_loaded, made, plc_ms(t0, t1), plc_ms(t1, t2));
                    else
                        plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：没写成（%s）。下一局照旧要慢一次，本局不受影响", why);
                } else {
                    plc_logf(NVN_PLAT_LOG_WARN, "管线缓存：取数据失败（VkResult %d），不落盘", (int)vr);
                }
                free(buf);
            }
        }
    }
    vkDestroyPipelineCache(g.d, pc, NULL);
}

// ---------------------------------------------------------------- 打开 / 关闭 / 能力

nvn_gpu_rc nvn_gpu_open(const nvn_gpu_open_desc *d, nvn_gpu_err *err) {
    if (g_open) return fail(err, NVN_GPU_E_ORDER, "已有打开的后端");
    if (!d || !d->host_device) return fail(err, NVN_GPU_E_ARG, "host_device 为空");
    struct s2a_vk_device *dev = d->host_device;
    if (!dev->instance || !dev->physical || !dev->device || !dev->queue)
        return fail(err, NVN_GPU_E_ARG, "host_device 不是已建好的 Vulkan 设备上下文");

    memset(&g, 0, sizeof g);
    g.dev = dev;
    {   // 诊断（nvk_mem_types，缺省 0）：打印显存类型表，判「有没有 HOST_CACHED 的可见类型」——
        // CPU 写映射内存与显卡抢带宽的那条路（见坑记录续 21）要不要改走 cached 暂存，取决于这张表。
        static int mt_on = -1; if (mt_on < 0) mt_on = nvn_plat_flag("nvk_mem_types", 0) != 0;
        if (mt_on) {
            const VkPhysicalDeviceMemoryProperties *mp = &dev->memory;
            for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
                char lb[200];
                snprintf(lb, sizeof lb, "显存类型 %u：标志 0x%x（DEVICE_LOCAL %d / HOST_VISIBLE %d / HOST_COHERENT %d / HOST_CACHED %d / LAZILY %d）、堆 %u",
                         i, (unsigned)mp->memoryTypes[i].propertyFlags,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0,
                         (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0,
                         (unsigned)mp->memoryTypes[i].heapIndex);
                nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", lb);
            }
        }
    }
    g.d = dev->device;
    vk_dq_init(g.d);
    g.phys = dev->physical;
    g.queue = dev->queue;
    if (!g_cv_ready) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // 与 deadline_after 同一时钟
        if (pthread_cond_init(&g_cv, &ca) != 0) {
            pthread_condattr_destroy(&ca);
            return fail(err, NVN_GPU_E_DEVICE, "cond 初始化失败");
        }
        pthread_condattr_destroy(&ca);
        g_cv_ready = 1;
    }
    fill_caps(&g, d);
    if (pthread_create(&g.worker, NULL, worker_main, &g) != 0)
        return fail(err, NVN_GPU_E_DEVICE, "worker 线程创建失败");
    g.worker_started = 1;
    pthread_mutex_lock(&g_seg_mu); g_seg_closed = 0; pthread_mutex_unlock(&g_seg_mu);
    plc_open(d->cache_dir);   // 缓存是加速不是前提：里面任何一步失败都落到「不带缓存」，不让 open 失败
    g_open = 1;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_caps_get(nvn_gpu_caps *out) {
    if (!out) return;
    if (!g_open) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&g_mu);
    *out = g.caps;
    pthread_mutex_unlock(&g_mu);
}

uint32_t nvn_gpu_format_caps(uint32_t nvn_format) {
    if (!g_open) return 0;
    nvn_fmt_info info;
    int expand = nvn_fmt_info_of(nvn_format, &info) && info.expand;
    VkFormat f = nvn_vk_tex_format(nvn_format);
    if (f == VK_FORMAT_UNDEFINED) return 0;
    int bcdec = 0;   // 这个块压缩格式在本设备上走计算解码
    if (nvn_format >= 0x41 && nvn_format <= 0x50 && (g.dev->bc_decode_mask >> (nvn_format - 0x41)) & 1u) {
        if (!nvn_vk_bcdec_route(nvn_format, NULL, &f, NULL, NULL, NULL)) return 0;
        bcdec = 1;
    }
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &p);
    VkFormatFeatureFlags o = p.optimalTilingFeatures;
    uint32_t out = 0;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) out |= NVN_GPU_FMT_SAMPLE;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) out |= NVN_GPU_FMT_FILTER;
    if (o & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) out |= NVN_GPU_FMT_RENDER;
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) out |= NVN_GPU_FMT_BLEND;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) out |= NVN_GPU_FMT_STORAGE;
    if (nvn_vk_format_is_depth(f)) out |= NVN_GPU_FMT_DEPTH;
    if (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) out |= NVN_GPU_FMT_ATOMIC;
    if (o & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) out |= NVN_GPU_FMT_VERTEX;
    if (nvn_vk_format_substituted(nvn_format)) out |= NVN_GPU_FMT_SUBST;   // #398：D24 换 D32 存储
    if (expand) {
        out |= NVN_GPU_FMT_SUBST;   // 存储被换成展开目标，逐字节拷贝要核心换算
        VkFormat sf = nvn_vk_format(nvn_format);
        VkFormatProperties sp;
        memset(&sp, 0, sizeof sp);
        if (sf != VK_FORMAT_UNDEFINED) vkGetPhysicalDeviceFormatProperties(g.phys, sf, &sp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) out &= ~NVN_GPU_FMT_VERTEX;
    }
    // 走计算解码的块压缩格式：能力是照「解码后的格式」查的，但它只能被采样——建图入口不收渲染 / 存储用途。
    // 把解码后格式的「可存储」原样报出去，核心会给带 IMAGE 标志的压缩贴图加存储用途，建图就被拒（#697：Mali 上 5 种格式的贴图全建不出来，背景全黑）。
    if (bcdec) out &= NVN_GPU_FMT_SAMPLE | NVN_GPU_FMT_FILTER;
    return out;
}

static void wsi_close_locked(void);   // backend_vulkan_wsi.inc
static void clr_close(void);          // backend_vulkan_clear.inc
nvn_gpu_rc nvn_gpu_close(uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return rc(NVN_GPU_OK);
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许 close");
    pthread_mutex_lock(&g_mu);
    if (g.close_active) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "另一个线程正在 close");
    }
    g.close_active = 1;
    g.closing = 1;
    pthread_mutex_unlock(&g_mu);
    uint64_t deadline = add_sat(now_ns(), timeout_ns);

    // ① 丢弃未提交 cmd：原子摘链后由本线程独占销毁，避免与并发 abort 重复释放。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s *list = g.live;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (list) {
        struct nvn_gpu_cmd_s *next = list->live_next;
        list->live_next = NULL;
        finish_cmd(list, 0);
        list = next;
    }

    // ②③④ 等在途段 / 退休队列 / 并发 commit 全部结束。
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (!g.qhead && !g.busy && !g.rhead && g.commit_active == 0) {
            wsi_close_locked();   // 在途段都完成了：还接着的面连同交换链一起收掉
            // 先销毁视图、放下其持有的底层缓冲引用，再由 worker 完成退休回调。
            while (g.views) destroy_view_locked(&g, g.views);
            if (!g.rhead) break;
        }
        uint64_t now = now_ns();
        if (now >= deadline) {
            g.close_active = 0;
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_TIMEOUT, "close 时限内未完成在途段 / 退休 / 提交（保留资源，可再调一次）");
        }
        struct timespec ts;
        deadline_after(&ts, deadline - now);
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_submit_mu);
    uint64_t now = now_ns();
    int bc_wait = nvn_vk_bcdec_wait(g.dev, g.bc, now < deadline ? deadline - now : 0);
    if (bc_wait != 0) {
        pthread_mutex_unlock(&g_submit_mu);
        pthread_mutex_lock(&g_mu); g.close_active = 0; pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_TIMEOUT, "close 时限内 BC 解码未证明完成（保留资源，可再调一次）");
    }
    while (g.bc_pending) {
        struct bc_temp *b = g.bc_pending; g.bc_pending = b->next;
        nvn_vk_bcdec_rec_free(g.dev, &b->rec);
        if (b->image) vkDestroyImage(g.d, b->image, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
    }
    pthread_mutex_unlock(&g_submit_mu);
    pthread_mutex_lock(&g_mu);
    g.stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g.worker, NULL);
    g.worker_started = 0;

    // 在途段都完成了、不会再有新管线：把驱动管线缓存写盘（失败只记日志）再销毁它。放在销毁管线之前，后面的拆除出事也不丢这一局的编译结果。
    // 在飞的局中写（#868）要先等它收尾，等多久算进这次 close 的截止时间；到点没写完就放弃收尾这次写，不拖 close。
    plc_close(deadline);

    // ⑤ 销毁所有仍活着的后端对象；已 release 的 retired 已由 worker 调完。
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *b = g.bufs;
    g.bufs = NULL;
    g.live = NULL;
    pthread_mutex_unlock(&g_mu);
    while (b) {
        struct nvn_gpu_buf_s *next = b->all_next;
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        if (b->buffer) vkDestroyBuffer(g.d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g.d, b->memory, NULL);
        free(b);
        b = next;
    }
    clr_close();
    struct nvn_gpu_pipe_s *p = g.pipes;
    g.pipes = NULL;
    memset(g_pipe_ht, 0, sizeof g_pipe_ht);
    while (p) {
        struct nvn_gpu_pipe_s *pn = p->all_next;
        destroy_pipe_objects(p);
        p = pn;
    }
    struct nvn_gpu_shader_s *sh = g.shaders;
    g.shaders = NULL;
    while (sh) {
        struct nvn_gpu_shader_s *sn = sh->all_next;
        if (sh->mod) vkDestroyShaderModule(g.d, sh->mod, NULL);
        free(sh->ents);
        free(sh->tfmt);
        free(sh);
        sh = sn;
    }
    struct nvn_gpu_samp_s *sp = g.samps;
    g.samps = NULL;
    while (sp) {
        struct nvn_gpu_samp_s *sn = sp->all_next;
        if (sp->int_sampler && sp->int_sampler != sp->sampler) vkDestroySampler(g.d, sp->int_sampler, NULL);
        if (sp->sampler) vkDestroySampler(g.d, sp->sampler, NULL);
        free(sp);
        sp = sn;
    }
    pc_destroy_all();
    struct nvn_gpu_view_s *vw = g.views;
    g.views = NULL;
    while (vw) {
        struct nvn_gpu_view_s *vn = vw->all_next;
        if (vw->att) vkDestroyImageView(g.d, vw->att, NULL);
        if (vw->view) vkDestroyImageView(g.d, vw->view, NULL);
        free(vw);
        vw = vn;
    }
    struct nvn_gpu_tex_s *tx = g.texs;
    g.texs = NULL;
    free(g_newimg); g_newimg = NULL; g_newimg_n = g_newimg_cap = 0;
    while (tx) {
        struct nvn_gpu_tex_s *tn = tx->all_next;
        if (tx->view) vkDestroyImageView(g.d, tx->view, NULL);
        if (tx->image) vkDestroyImage(g.d, tx->image, NULL);
        if (tx->memory) vkFreeMemory(g.d, tx->memory, NULL);
        if (tx->bc_sbuf) vkDestroyBuffer(g.d, tx->bc_sbuf, NULL);   // 现解贴图的压缩影子（同 destroy_tex_locked）
        if (tx->bc_smem) vkFreeMemory(g.d, tx->bc_smem, NULL);
        free(tx);
        tx = tn;
    }
    texmem_drain(g.d);
    seg_drain();
    if (g.bc) nvn_vk_bcdec_destroy(g.dev, g.bc);
    free(g.submitted);
    g.submitted = NULL;
    g.nsub = g.capsub = 0;
    g.closed = 1;
    g.close_active = 0;
    g_open = 0;
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 缓冲

static int is_shared(int mem) { return mem == NVN_GPU_MEM_SHARED; }

static int session_accepting(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = !g.closing;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// prefer_cached：主机可见的缓冲优先挑 HOST_CACHED 的显存类型（本机有 DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED，类型 6）。
// 暂存缓冲（每帧约 9 MB 常量缓冲 + 顶点镜像落盘）是 CPU 反复小段写的：写在缓存内存上比写在不缓存映射上快数倍，
// 且该类型带 coherent 位、不需要显式 flush（见坑记录续 21 的带宽争用读数）。
static nvn_gpu_rc buf_create_impl(uint64_t bytes, int mem, const void *init, int prefer_cached, nvn_gpu_buf *out, nvn_gpu_err *err);
nvn_gpu_rc nvn_gpu_buf_create(uint64_t bytes, int mem, const void *init, nvn_gpu_buf *out, nvn_gpu_err *err) {
    // 开关 nvk_mem_cached：所有主机可见缓冲（含池镜像）也走 HOST_CACHED 类型。缺省跟随重叠档（nvk_sync_lag≠0 时开）：
    // 默认档 A/B 中性（绑定 5.2 / 回放 33.2 / GPU 忙 39.0–39.5），而重叠档里 CPU 直写镜像 550 KB/帧与显卡抢带宽，
    // 开了之后顶点流 22–25 → 17.9 毫秒、帧 p50 52 → 50（读数见坑记录续 23）。
    static int all_cached = -1; static unsigned k;
    if (all_cached < 0 || !(++k & 255)) all_cached = nvn_plat_flag("nvk_mem_cached", nvn_plat_flag("nvk_sync_lag", 0) != 0) != 0;
    return buf_create_impl(bytes, mem, init, all_cached, out, err);
}
static nvn_gpu_rc buf_create_impl(uint64_t bytes, int mem, const void *init, int prefer_cached, nvn_gpu_buf *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    if (bytes == 0 || bytes > g.caps.max_buf_bytes)
        return fail(err, NVN_GPU_E_ARG, "申请 %llu 字节：超出单次分配上限 %llu（maxMemoryAllocationSize；SSBO 描述符范围上限 %llu 是另一项）",
                    (unsigned long long)bytes, (unsigned long long)g.caps.max_buf_bytes, (unsigned long long)g.caps.max_ssbo_range);
    if (mem != NVN_GPU_MEM_SHARED && mem != NVN_GPU_MEM_DEVICE) return fail(err, NVN_GPU_E_ARG, "mem 非法");
    if (mem == NVN_GPU_MEM_DEVICE && init) return fail(err, NVN_GPU_E_ARG, "DEVICE 内存不能带 init");

    struct nvn_gpu_buf_s *b = calloc(1, sizeof *b);
    if (!b) return fail(err, NVN_GPU_E_OOM, "缓冲结构分配失败");
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.d, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkCreateBuffer 失败（%llu 字节）", (unsigned long long)bytes);
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.d, b->buffer, &req);
    VkMemoryPropertyFlags want = is_shared(mem) ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = UINT32_MAX;
    if (prefer_cached && is_shared(mem))
        type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, want, NULL);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "找不到满足要求的显存类型");
    }
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.d, &ai, NULL, &b->memory) != VK_SUCCESS) {
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateMemory 失败");
    }
    if (vkBindBufferMemory(g.d, b->buffer, b->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(g.d, b->memory, NULL);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        free(b);
        return fail(err, NVN_GPU_E_OOM, "vkBindBufferMemory 失败");
    }
    b->bytes = bytes;
    b->mem_kind = mem;
    if (is_shared(mem)) {
        if (vkMapMemory(g.d, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->mapped) != VK_SUCCESS) {
            vkFreeMemory(g.d, b->memory, NULL);
            vkDestroyBuffer(g.d, b->buffer, NULL);
            free(b);
            return fail(err, NVN_GPU_E_OOM, "vkMapMemory 失败");
        }
        if (init) memcpy(b->mapped, init, (size_t)bytes);
    }
    pthread_mutex_lock(&g_mu);
    if (g.closing || g.closed) {   // 与 close 的快照串行：close 开始后新建的对象不留在账上
        pthread_mutex_unlock(&g_mu);
        if (b->mapped) vkUnmapMemory(g.d, b->memory);
        vkDestroyBuffer(g.d, b->buffer, NULL);
        vkFreeMemory(g.d, b->memory, NULL);
        free(b);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    b->all_next = g.bufs;
    g.bufs = b;
    g.stats[STAT_BUFS]++;
    pthread_mutex_unlock(&g_mu);
    *out = b;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_buf_wrap(void *host, uint64_t bytes, nvn_gpu_buf *out, nvn_gpu_err *err) {
    (void)host;
    (void)bytes;
    if (out) *out = NULL;
    return fail(err, NVN_GPU_E_UNSUPPORTED, "本设备 / 本段未启用 HOST_IMPORT（VK_EXT_external_memory_host 未实测）");
}

uint64_t nvn_gpu_buf_va(nvn_gpu_buf b) { return b ? b->va : 0; }   // 未开 CAP_GPU_VA：恒 0
uint8_t *nvn_gpu_buf_cpu(nvn_gpu_buf b) { return b ? b->mapped : NULL; }

void nvn_gpu_buf_release(nvn_gpu_buf bh, nvn_gpu_retired_fn retired, void *user) {
    if (!bh) return;
    struct nvn_gpu_buf_s *b = bh;
    pthread_mutex_lock(&g_mu);
    if (!b->released) {
        b->released = 1;
        b->retired = retired;
        b->retired_user = user;
        if (b->refs == 0) queue_retire_locked(&g, b);
    }
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_buf_use(nvn_gpu_cmd ch, nvn_gpu_buf bh) {
    if (!ch || !bh) return;
    cmd_ref((struct nvn_gpu_cmd_s *)ch, bh, REF_BUF);
}

// ---------------------------------------------------------------- arena

static struct nvn_gpu_buf_s *new_staging(struct nvn_gpu_cmd_s *c, uint64_t cap) {
    nvn_gpu_err e;
    nvn_gpu_buf b = seg_reuse_on() ? seg_take_stg(cap) : NULL;
    static int cached_on = -1; if (cached_on < 0) cached_on = nvn_plat_flag("nvk_stage_cached", 1) != 0;
    if (!b && !rc_is(buf_create_impl(cap, NVN_GPU_MEM_SHARED, NULL, cached_on, &b, &e), NVN_GPU_OK)) return NULL;
    if (c->nstaging == c->scap) {
        uint32_t n = c->scap ? c->scap * 2 : 4;
        struct vk_staging *p = realloc(c->staging, (size_t)n * sizeof *p);
        if (!p) {
            nvn_gpu_buf_release(b, NULL, NULL);
            return NULL;
        }
        c->staging = p;
        c->scap = n;
    }
    c->staging[c->nstaging++].buf = b;
    return b;
}

nvn_gpu_rc nvn_gpu_arena_alloc(nvn_gpu_cmd ch, uint64_t bytes, uint32_t align, nvn_gpu_slice *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    memset(out, 0, sizeof *out);
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return fail(err, c->sticky.code, "%s", c->why);
    if (!g_open || g.closing) return fail(err, NVN_GPU_E_STOPPED, "会话已开始关闭");
    if (bytes == 0) return fail(err, NVN_GPU_E_ARG, "bytes 为 0");
    if (align == 0) align = 4;
    if (align & (align - 1)) return fail(err, NVN_GPU_E_ARG, "align 不是 2 的幂：%u", align);
    uint64_t off = (c->arena_used + align - 1) & ~(uint64_t)(align - 1);
    if (!c->arena || off + bytes > c->arena_cap) {
        uint64_t cap = bytes + align;
        if (cap < VK_ARENA_MIN) cap = VK_ARENA_MIN;
        struct nvn_gpu_buf_s *b = new_staging(c, cap);
        if (!b) return fail(err, NVN_GPU_E_OOM, "arena %llu 字节分配失败", (unsigned long long)cap);
        c->arena = b;
        c->arena_cap = cap;
        c->arena_used = 0;
        off = 0;
    }
    out->buf = c->arena;
    out->off = off;
    out->cpu = c->arena->mapped + off;
    out->va = 0;
    c->arena_used = off + bytes;
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 录制原语

// 保守的全局内存屏障：传输操作前后与段末各来一次（先保证正确，后面再按访问类型收窄）。
static void mem_barrier(struct nvn_gpu_cmd_s *c, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(c->cb, src_stage, dst_stage, 0, 1, &b, 0, NULL, 0, NULL);
}
static void full_barrier(struct nvn_gpu_cmd_s *c) {
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    c->xfer_dirty = 0;
}


static int buf_range(const struct nvn_gpu_buf_s *b, uint64_t off, uint64_t bytes) {
    return b && off <= b->bytes && bytes <= b->bytes - off;
}

void nvn_gpu_buf_write(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, const void *src, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || (!src && bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 句柄 / src 为空"); return; }
    if (!buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "buf_write 越界 off=%llu bytes=%llu size=%llu",
                                                   (unsigned long long)off, (unsigned long long)bytes, (unsigned long long)b->bytes); return; }
    if (bytes == 0) return;
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    if (!rc_is(nvn_gpu_arena_alloc(ch, bytes, 4, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "buf_write 暂存：%s", e.why);
        return;
    }
    memcpy(sl.cpu, src, (size_t)bytes);
    full_barrier(c);
    VkBufferCopy bc = {sl.off, off, bytes};
    vkCmdCopyBuffer(c->cb, sl.buf->buffer, b->buffer, 1, &bc);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, b, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_WRITES]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_fill_buf(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t bytes, uint32_t value) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *b = bh;
    if (!b || !buf_range(b, off, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 越界"); return; }
    if ((off & 3) || (bytes & 3)) { set_sticky(c, NVN_GPU_E_ARG, "fill_buf 需 4 字节对齐 off=%llu bytes=%llu",
                                                  (unsigned long long)off, (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    vkCmdFillBuffer(c->cb, b->buffer, off, bytes, value);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, b, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_FILLS]++;
    pthread_mutex_unlock(&g_mu);
}

void nvn_gpu_copy_buf(nvn_gpu_cmd ch, nvn_gpu_buf src, uint64_t soff, nvn_gpu_buf dst, uint64_t doff, uint64_t bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    if (!ch) return;
    struct nvn_gpu_cmd_s *c = ch;
    if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_buf_s *s = src, *d = dst;
    if (!s || !d || !buf_range(s, soff, bytes) || !buf_range(d, doff, bytes)) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 越界"); return; }
    if (bytes & 3) { set_sticky(c, NVN_GPU_E_ARG, "copy_buf 需 4 字节长度（本段实现限制）bytes=%llu", (unsigned long long)bytes); return; }
    if (bytes == 0) return;
    full_barrier(c);
    VkBufferCopy bc = {soff, doff, bytes};
    vkCmdCopyBuffer(c->cb, s->buffer, d->buffer, 1, &bc);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    cmd_ref(c, s, REF_BUF);
    cmd_ref(c, d, REF_BUF);
    pthread_mutex_lock(&g_mu);
    g.stats[STAT_COPIES]++;
    pthread_mutex_unlock(&g_mu);
}


// ---------------------------------------------------------------- 着色器 / 管线 / 通道 / 绘制（纹理三角形）

static VkShaderStageFlagBits vk_shader_stage(nvn_gpu_stage st) {
    switch (st) {
    case NVN_GPU_SH_VS: return VK_SHADER_STAGE_VERTEX_BIT;
    case NVN_GPU_SH_FS: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case NVN_GPU_SH_GS: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case NVN_GPU_SH_CS: return VK_SHADER_STAGE_COMPUTE_BIT;
    default: return 0;
    }
}

static VkPrimitiveTopology vk_topology(uint32_t prim) {
    switch (prim) {
    case 0x0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 0x5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // 2 线环 / 7+：未实现
    }
}

static VkBlendFactor vk_blend_factor(uint32_t f) {
    switch (f) {
    case 0x1: return VK_BLEND_FACTOR_ZERO;
    case 0x2: return VK_BLEND_FACTOR_ONE;
    case 0x3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_MAX_ENUM;
    }
}
static VkBlendOp vk_blend_op(uint32_t e) {
    switch (e) {
    case 0x1: return VK_BLEND_OP_ADD;
    case 0x2: return VK_BLEND_OP_SUBTRACT;
    case 0x3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x4: return VK_BLEND_OP_MIN;
    case 0x5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_MAX_ENUM;
    }
}
// 逐项表达 NVN 语义，不依赖与 Vulkan 枚举编号碰巧一致。
static VkLogicOp vk_logic_op(uint32_t op) {
    switch (op) {
    case NVN_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case NVN_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case NVN_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case NVN_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
    case NVN_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case NVN_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case NVN_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case NVN_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case NVN_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case NVN_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case NVN_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case NVN_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case NVN_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case NVN_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    case NVN_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case NVN_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    default: return VK_LOGIC_OP_MAX_ENUM;
    }
}

static VkStencilOp vk_stencil_op(uint32_t op) {   // NVN_STENCIL_OP_*：1 KEEP … 8 DECR_WRAP
    switch (op) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp vk_depth_func(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkDescriptorType vk_desc_type(uint8_t cls) {
    switch (cls) {
    case NVN_GPU_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NVN_GPU_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NVN_GPU_RES_TEX: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case NVN_GPU_RES_SAMP: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case NVN_GPU_RES_TEX_SAMP: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case NVN_GPU_RES_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case NVN_GPU_RES_TBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case NVN_GPU_RES_IMAGE_TBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

// ---------------- SPIR-V texel 反射（只读 blob；未知格式由绑定的 view 提供） ----------------
// 只解析 TBUF / IMAGE_TBUF 需要的最小指令子集：
//   OpDecorate(Binding/DescriptorSet) → OpVariable(UniformConstant) → OpTypePointer → (OpTypeArray) → OpTypeImage。
// 约定（v2 未定，报 PR 请契约属主复核）：
//   · OpTypeImage.Dim 必须为 Buffer（一维缓冲纹理只能映射成 texel buffer）；
//   · TBUF 收 Sampled=0/1（采样读），IMAGE_TBUF 收 Sampled=0/2（存储写）；
//   · ImageFormat 非 Unknown 按 SPIR-V 值映射；Unknown 留给绑定视图，不猜格式；
//   · 项与 blob 对不上 → E_LAYOUT（逐项核对的 A1 要求）；数值类 / 位宽推不出 → E_UNSUPPORTED。
enum { SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_IMAGE = 25,
       SPV_OP_TYPE_SAMPLED_IMAGE = 27, SPV_OP_TYPE_ARRAY = 28, SPV_OP_TYPE_RUNTIME_ARRAY = 29,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_VARIABLE = 59, SPV_OP_DECORATE = 71 };
enum { SPV_DEC_BINDING = 33, SPV_DEC_SET = 34 };
enum { SPV_DIM_BUFFER = 5, SPV_SC_UNIFORM_CONST = 0 };   // SPIR-V Dim：0-3 维度、4=Rect、5=Buffer

struct spv_id {
    uint8_t kind;          // 1 int 2 float 3 vector 4 image 5 array 7 pointer 8 variable 9 sampled_image
    uint8_t sgn, sampled, dim;
    uint32_t a;            // int/float: 位宽；vector: 分量类型 id；image: sampled 类型 id；array: elem id；ptr: pointee id；var: 指针类型 id
    uint32_t fmt;          // image: ImageFormat 操作数
};

// SPIR-V ImageFormat 操作数（非 Unknown）→ VkFormat
static VkFormat spv_img_fmt(uint32_t f) {
    switch (f) {
    case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 3: return VK_FORMAT_R32_SFLOAT;
    case 4: return VK_FORMAT_R8G8B8A8_UNORM;
    case 5: return VK_FORMAT_R8G8B8A8_SNORM;
    case 6: return VK_FORMAT_R32G32_SFLOAT;
    case 7: return VK_FORMAT_R16G16_SFLOAT;
    case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 9: return VK_FORMAT_R16_SFLOAT;
    case 10: return VK_FORMAT_R16G16B16A16_UNORM;
    case 11: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 12: return VK_FORMAT_R16G16_UNORM;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R16_UNORM;
    case 15: return VK_FORMAT_R8_UNORM;
    case 16: return VK_FORMAT_R16G16B16A16_SNORM;
    case 17: return VK_FORMAT_R16G16_SNORM;
    case 18: return VK_FORMAT_R8G8_SNORM;
    case 19: return VK_FORMAT_R16_SNORM;
    case 20: return VK_FORMAT_R8_SNORM;
    case 21: return VK_FORMAT_R32G32B32A32_SINT;
    case 22: return VK_FORMAT_R16G16B16A16_SINT;
    case 23: return VK_FORMAT_R8G8B8A8_SINT;
    case 24: return VK_FORMAT_R32_SINT;
    case 25: return VK_FORMAT_R32G32_SINT;
    case 26: return VK_FORMAT_R16G16_SINT;
    case 27: return VK_FORMAT_R8G8_SINT;
    case 28: return VK_FORMAT_R16_SINT;
    case 29: return VK_FORMAT_R8_SINT;
    case 30: return VK_FORMAT_R32G32B32A32_UINT;
    case 31: return VK_FORMAT_R16G16B16A16_UINT;
    case 32: return VK_FORMAT_R8G8B8A8_UINT;
    case 33: return VK_FORMAT_R32_UINT;
    case 34: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 35: return VK_FORMAT_R32G32_UINT;
    case 36: return VK_FORMAT_R16G16_UINT;
    case 37: return VK_FORMAT_R8G8_UINT;
    case 38: return VK_FORMAT_R16_UINT;
    case 39: return VK_FORMAT_R8_UINT;
    case 40: return VK_FORMAT_R64_UINT;
    case 41: return VK_FORMAT_R64_SINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Vulkan 1.1 设备级 without-format 特性适用的格式；1..39 与规范列表对应。
static int texel_unknown_storage_supported(VkFormat f) {
    for (uint32_t i = 1; i <= 39; i++)
        if (spv_img_fmt(i) == f) return 1;
    return 0;
}

static nvn_gpu_rc spv_texel_formats(const void *blob, size_t bytes,
                                    const nvn_gpu_layout_ent *ents, uint32_t nent, VkFormat *tfmt,
                                    uint32_t *required, nvn_gpu_err *err) {
    const uint32_t *w = blob;
    if (bytes < 20 || w[0] != 0x07230203u) return fail(err, NVN_GPU_E_ARG, "SPIR-V 头非法");
    uint32_t bound = w[3];
    if (!bound || bound > 0x100000) return fail(err, NVN_GPU_E_ARG, "SPIR-V id 上界非法：%u", bound);
    struct spv_id *ids = calloc(bound, sizeof *ids);
    uint32_t *vset = malloc(bound * sizeof *vset), *vbind = malloc(bound * sizeof *vbind);
    if (!ids || !vset || !vbind) {
        free(ids); free(vset); free(vbind);
        return fail(err, NVN_GPU_E_OOM, "SPIR-V 反射暂存分配失败");
    }
    for (uint32_t i = 0; i < bound; i++) vset[i] = vbind[i] = UINT32_MAX;
    size_t pos = 5, n = bytes / 4;
    while (pos < n) {
        uint32_t w0 = w[pos], op = w0 & 0xFFFFu, wc = w0 >> 16;
        if (!wc || pos + wc > n) break;
        const uint32_t *o = w + pos + 1;
        switch (op) {
        case SPV_OP_CAPABILITY:
            if (wc >= 2 && o[0] == 55) *required |= NVN_GPU_ACC_R;
            if (wc >= 2 && o[0] == 56) *required |= NVN_GPU_ACC_W;
            break;
        case SPV_OP_TYPE_INT:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 1; t->a = o[1]; t->sgn = (uint8_t)o[2]; }
            break;
        case SPV_OP_TYPE_FLOAT:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 2; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_VECTOR:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 3; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_IMAGE:
            if (wc >= 9 && o[0] < bound) {
                struct spv_id *t = &ids[o[0]];
                t->kind = 4; t->a = o[1]; t->dim = (uint8_t)o[2]; t->sampled = (uint8_t)o[6]; t->fmt = o[7];
            }
            break;
        case SPV_OP_TYPE_SAMPLED_IMAGE:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 9; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_ARRAY:
        case SPV_OP_TYPE_RUNTIME_ARRAY:
            if (wc >= 3 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 5; t->a = o[1]; }
            break;
        case SPV_OP_TYPE_POINTER:
            if (wc >= 4 && o[0] < bound) { struct spv_id *t = &ids[o[0]]; t->kind = 7; t->a = o[2]; }
            break;
        case SPV_OP_VARIABLE:
            if (wc >= 4 && o[1] < bound && o[2] == SPV_SC_UNIFORM_CONST) {
                struct spv_id *t = &ids[o[1]];
                t->kind = 8; t->a = o[0];
            }
            break;
        case SPV_OP_DECORATE:
            if (wc >= 4 && o[0] < bound) {
                if (o[1] == SPV_DEC_BINDING) vbind[o[0]] = o[2];
                else if (o[1] == SPV_DEC_SET) vset[o[0]] = o[2];
            }
            break;
        default: break;
        }
        pos += wc;
    }
    nvn_gpu_rc r = rc(NVN_GPU_OK);
    for (uint32_t i = 0; i < nent; i++) {
        const nvn_gpu_layout_ent *e = &ents[i];
        if (e->cls != NVN_GPU_RES_TBUF && e->cls != NVN_GPU_RES_IMAGE_TBUF) continue;
        uint32_t v = UINT32_MAX;
        for (uint32_t j = 0; j < bound; j++)
            if (ids[j].kind == 8 && vset[j] == e->set && vbind[j] == e->binding) { v = j; break; }
        if (v == UINT32_MAX) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 在 blob 里无对应 OpVariable", e->set, e->binding);
            break;
        }
        uint32_t t = ids[v].a, guard = 0;
        while (t < bound && (ids[t].kind == 7 || ids[t].kind == 5 || ids[t].kind == 9) && ++guard <= 16) t = ids[t].a;
        if (t >= bound || ids[t].kind != 4) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) 的类型链解不到 OpTypeImage", e->set, e->binding);
            break;
        }
        if (ids[t].dim != SPV_DIM_BUFFER) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Dim=%u 不是 Buffer", e->set, e->binding, ids[t].dim);
            break;
        }
        if (e->cls == NVN_GPU_RES_TBUF && ids[t].sampled > 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=%u 与 TBUF（采样读）不符", e->set, e->binding, ids[t].sampled);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && ids[t].sampled == 1) {
            r = fail(err, NVN_GPU_E_LAYOUT, "texel 布局项 (set %u, binding %u) Sampled=1 与 IMAGE_TBUF（存储写）不符", e->set, e->binding);
            break;
        }
        if (e->cls == NVN_GPU_RES_IMAGE_TBUF && !ids[t].fmt)
            *required |= e->access ? e->access & NVN_GPU_ACC_RW : NVN_GPU_ACC_RW;
        VkFormat f = ids[t].fmt ? spv_img_fmt(ids[t].fmt) : VK_FORMAT_UNDEFINED;
        if (ids[t].fmt && f == VK_FORMAT_UNDEFINED) {
            r = fail(err, NVN_GPU_E_UNSUPPORTED, "texel 布局项 (set %u, binding %u) 格式推不出（ImageFormat=%u）", e->set, e->binding, ids[t].fmt);
            break;
        }
        tfmt[i] = f;
    }
    free(ids);
    free(vset);
    free(vbind);
    return r;
}

static void destroy_shader_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_shader_s *sh) {
    struct nvn_gpu_shader_s **pp = &vk->shaders;
    while (*pp && *pp != sh) pp = &(*pp)->all_next;
    if (*pp) *pp = sh->all_next;
    if (sh->mod) vkDestroyShaderModule(vk->d, sh->mod, NULL);
    free(sh->ents);
    free(sh->tfmt);
    free(sh);
}

nvn_gpu_rc nvn_gpu_shader_load(const nvn_gpu_shader_src *s, nvn_gpu_shader *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!s || !s->blob || !s->bytes) return fail(err, NVN_GPU_E_ARG, "着色器 blob 为空");
    if (s->stage == NVN_GPU_SH_GS && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 未启用或探测未通过");
    if (s->kind != NVN_GPU_BLOB_SPIRV) return fail(err, NVN_GPU_E_UNSUPPORTED, "只收 SPIR-V blob（kind=%d）", s->kind);
    if (s->bytes & 3) return fail(err, NVN_GPU_E_ARG, "SPIR-V 字节数不是 4 的倍数");
    struct nvn_gpu_shader_s *sh = calloc(1, sizeof *sh);
    if (!sh) return fail(err, NVN_GPU_E_OOM, "着色器结构分配失败");
    sh->stage = s->stage;
    snprintf(sh->entry, sizeof sh->entry, "%s", s->entry && s->entry[0] ? s->entry : "main");
    if (s->layout) {
        sh->nent = s->layout->nent;
        sh->push_off = s->layout->push_off;
        sh->push_bytes = s->layout->push_bytes;
        if (sh->nent) {
            sh->ents = calloc(sh->nent, sizeof *sh->ents);
            if (!sh->ents) {
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "布局拷贝分配失败");
            }
            memcpy(sh->ents, s->layout->ent, (size_t)sh->nent * sizeof *sh->ents);
        }
        int need_tfmt = 0;
        for (uint32_t i = 0; i < sh->nent; i++)
            if (sh->ents[i].cls == NVN_GPU_RES_TBUF || sh->ents[i].cls == NVN_GPU_RES_IMAGE_TBUF) need_tfmt = 1;
        if (need_tfmt) {
            sh->tfmt = calloc(sh->nent, sizeof *sh->tfmt);
            if (!sh->tfmt) {
                free(sh->ents);
                free(sh);
                return fail(err, NVN_GPU_E_OOM, "texel 格式表分配失败");
            }
            uint32_t required = 0;
            nvn_gpu_rc tr = spv_texel_formats(s->blob, s->bytes, sh->ents, sh->nent, sh->tfmt, &required, err);
            if (!rc_is(tr, NVN_GPU_OK)) {
                free(sh->tfmt);
                free(sh->ents);
                free(sh);
                return tr;
            }
            sh->missing_storage_format_feature =
                ((required & NVN_GPU_ACC_R) && !g.dev->core_features.shaderStorageImageReadWithoutFormat) ||
                ((required & NVN_GPU_ACC_W) && !g.dev->core_features.shaderStorageImageWriteWithoutFormat);
        }
    }
    // 没有启用能力时保留可查询的 shader 句柄，管线入口返回 UNSUPPORTED；
    // 不向驱动提交不合法 SPIR-V，以免 vkCreateShaderModule 先触发 validation。
    if (!sh->missing_storage_format_feature) {
        VkShaderModuleCreateInfo ci = {0};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = s->bytes; ci.pCode = s->blob;
        if (vkCreateShaderModule(g.d, &ci, NULL, &sh->mod) != VK_SUCCESS) {
            free(sh->tfmt); free(sh->ents); free(sh);
            return fail(err, NVN_GPU_E_COMPILE, "vkCreateShaderModule 失败（stage %d）", s->stage);
        }
    }
    sh->refs = 1;   // 核心持有
    pthread_mutex_lock(&g_mu);
    sh->all_next = g.shaders;
    g.shaders = sh;
    pthread_mutex_unlock(&g_mu);
    *out = sh;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_shader_release(nvn_gpu_shader sh) {
    struct nvn_gpu_shader_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        if (s->refs > 0) s->refs--;
        if (s->refs == 0) destroy_shader_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

static VkRenderPass make_pipeline_pass(const nvn_gpu_pipe_desc *d) {
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    uint32_t natt = 0, ncol = 0;
    VkSampleCountFlagBits samples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);
    uint32_t nslot = 0;   // 颜色槽个数 = 最后一个有格式的槽 + 1；中间没格式的槽是空位（VK_ATTACHMENT_UNUSED）
    for (uint32_t i = 0; i < 8; i++) if (d->cfmt[i]) nslot = i + 1;
    for (uint32_t i = 0; i < nslot; i++) {
        if (!d->cfmt[i]) { col[ncol].attachment = VK_ATTACHMENT_UNUSED; col[ncol].layout = VK_IMAGE_LAYOUT_UNDEFINED; ncol++; continue; }
        VkFormat f = nvn_vk_tex_format(d->cfmt[i]);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col[ncol].attachment = natt;
        col[ncol].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ncol++;
        natt++;
    }
    int has_depth = 0;
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (d->dfmt) {
        VkFormat f = nvn_vk_tex_format(d->dfmt);
        if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
        memset(&att[natt], 0, sizeof att[natt]);
        att[natt].format = f;
        att[natt].samples = samples;
        att[natt].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[natt].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[natt].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[natt].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment = natt;
        has_depth = 1;
        natt++;
    }
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = has_depth ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = natt;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    OC(OC_RP);
    if (vkCreateRenderPass(g.d, &rp, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

static int same_spec(const nvn_gpu_stage_desc *a, const nvn_gpu_stage_desc *b) {
    if (a->sh != b->sh || a->nspec != b->nspec) return 0;
    return a->nspec == 0 || memcmp(a->spec, b->spec, (size_t)a->nspec * sizeof *a->spec) == 0;
}

// 图形管线按描述散列成桶。原先在全部管线的一条链表里逐条比：一款 3D 游戏几百条管线、每帧取 513 次，
// 真机上管线数翻一倍「开通道与取管线」每帧就多 1.3 毫秒（同一局里两种写法的管线并存时量到的）。
// 散列只取描述里一部分字段（都是 same_pipe_desc 逐字节 / 逐值比的），相等与否仍由 same_pipe_desc 定。
static uint32_t pipe_hash(const nvn_gpu_pipe_desc *d) {
    uint64_t h = 1469598103934665603ull;
#define PH(p, n) do { const uint8_t *q_ = (const uint8_t *)(p); for (size_t i_ = 0; i_ < (size_t)(n); i_++) h = (h ^ q_[i_]) * 1099511628211ull; } while (0)
    for (int i = 0; i < 5; i++) { uintptr_t sh = (uintptr_t)d->st[i].sh; PH(&sh, sizeof sh); }
    uint32_t v[4] = {(uint32_t)d->prim, (uint32_t)d->dfmt, (uint32_t)d->sfmt, (uint32_t)d->samples}; PH(v, sizeof v);
    PH(d->cfmt, sizeof d->cfmt); PH(d->blend, sizeof d->blend); PH(&d->color, sizeof d->color); PH(&d->chmask, sizeof d->chmask);
    PH(&d->ds, sizeof d->ds);
    {   // poly_st 的位域只占部分对象位；散列和相等判定都不能依赖填充位。
        uint8_t poly[] = {d->poly.cull, d->poly.front, d->poly.mode, d->poly.offset_en};
        PH(poly, sizeof poly);
    }
#undef PH
    return (uint32_t)(h ^ h >> 32);
}

static int same_pipe_desc(const nvn_gpu_pipe_desc *a, const nvn_gpu_pipe_desc *b) {
    if (a->prim != b->prim || a->patch_size != b->patch_size || a->dfmt != b->dfmt || a->sfmt != b->sfmt ||
        a->samples != b->samples || a->rast_discard != b->rast_discard || a->clip_neg1 != b->clip_neg1)
        return 0;
    for (int i = 0; i < 5; i++)
        if (!same_spec(&a->st[i], &b->st[i])) return 0;
    for (int i = 0; i < 8; i++)
        if (a->cfmt[i] != b->cfmt[i]) return 0;
    return memcmp(a->lmap, b->lmap, sizeof a->lmap) == 0 &&
           memcmp(a->blend, b->blend, sizeof a->blend) == 0 &&
           memcmp(&a->color, &b->color, sizeof a->color) == 0 &&
           memcmp(&a->chmask, &b->chmask, sizeof a->chmask) == 0 &&
           memcmp(&a->ms, &b->ms, sizeof a->ms) == 0 &&
           memcmp(a->attr, b->attr, sizeof a->attr) == 0 &&
           memcmp(a->stream, b->stream, sizeof a->stream) == 0 &&
           memcmp(&a->ds, &b->ds, sizeof a->ds) == 0 &&
           a->poly.cull == b->poly.cull && a->poly.front == b->poly.front &&
           a->poly.mode == b->poly.mode && a->poly.offset_en == b->poly.offset_en &&
           a->origin_ll == b->origin_ll && a->depth_clamp == b->depth_clamp;
}

static void destroy_pipe_objects(struct nvn_gpu_pipe_s *p) {
    if (!p) return;
    if (p->pipe) vkDestroyPipeline(g.d, p->pipe, NULL);
    if (p->pass) vkDestroyRenderPass(g.d, p->pass, NULL);
    if (p->layout) vkDestroyPipelineLayout(g.d, p->layout, NULL);
    for (uint32_t i = 0; i < p->nset; i++)
        if (p->dsl[i]) vkDestroyDescriptorSetLayout(g.d, p->dsl[i], NULL);
    for (int i = 0; i < 6; i++) { free(p->stage_ents[i]); free(p->stage_tfmt[i]); }
    free(p);
}

nvn_gpu_rc nvn_gpu_pipe_get(const nvn_gpu_pipe_desc *d, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!d) return fail(err, NVN_GPU_E_ARG, "管线描述为空");
    // NVN POINT=0 / LINE=1 / FILL=2；完整 poly 已由 same_pipe_desc 纳入缓存键。
    if (d->poly.mode > 2) return fail(err, NVN_GPU_E_ARG, "未知多边形填充模式 %u", (unsigned)d->poly.mode);
    if (d->poly.mode != 2 && !g.dev->core_features.fillModeNonSolid)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "非填充多边形需要已启用的 fillModeNonSolid");
    // 片元阶段可以没有：只写深度的管线（深度预通道、阴影图），颜色附件上没有输出
    if (!d->st[NVN_GPU_SH_VS].sh)
        return fail(err, NVN_GPU_E_ARG, "图形管线至少需要顶点阶段");
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh && d->st[st].sh->missing_storage_format_feature)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    if (d->st[NVN_GPU_SH_GS].sh && !(g.caps.caps & NVN_GPU_CAP_GEOMETRY))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 未启用或探测未通过");
    if (d->st[NVN_GPU_SH_GS].sh && d->prim != 0 && d->prim != 1 && d->prim != 4)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "GS 非原生图元未实现");
    for (int i = NVN_GPU_SH_TCS; i < 5; i++)
        if (d->st[i].sh) return fail(err, NVN_GPU_E_UNSUPPORTED, "细分阶段：本段未实现");

    const uint32_t hb = pipe_hash(d) % PIPE_HT_N;
    pthread_mutex_lock(&g_mu);
    static int scan = -1; static uint32_t scan_n;   // 诊断开关 nvk_pipe_scan=1：走原来的整表逐条比（同一局里对比用），每 256 次取管线读一次
    if (scan < 0 || !(++scan_n & 255)) scan = nvn_plat_flag("nvk_pipe_scan", 0) != 0;
    struct timespec lk0; clock_gettime(CLOCK_MONOTONIC, &lk0); uint32_t steps = 0;
    for (struct nvn_gpu_pipe_s *q = scan ? g.pipes : g_pipe_ht[hb]; q; q = scan ? q->all_next : q->hnext) {
        steps++;
        if (same_pipe_desc(&q->key, d)) {
            { struct timespec lk1; clock_gettime(CLOCK_MONOTONIC, &lk1);   // 诊断读数：查找这一段用了多久、比了多少条（随对象读数那几行打）
              __atomic_fetch_add(&g_pl_ns, (uint64_t)(lk1.tv_sec - lk0.tv_sec) * 1000000000ull + (uint64_t)lk1.tv_nsec - (uint64_t)lk0.tv_nsec, __ATOMIC_RELAXED);
              __atomic_fetch_add(&g_pl_n, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_pl_steps, steps, __ATOMIC_RELAXED); }
            pthread_mutex_unlock(&g_mu);
            *out = q;   // 后端缓存持有，核心不 release
            return rc(NVN_GPU_OK);
        }
    }
    pthread_mutex_unlock(&g_mu);

    struct merged { uint8_t set, binding, cls; uint16_t count; uint32_t stages; } m[64];
    uint32_t nm = 0, maxset = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        for (uint32_t i = 0; i < sh->nent; i++) {
            const nvn_gpu_layout_ent *e = &sh->ents[i];
            if (e->cls == NVN_GPU_RES_VA)
                return fail(err, NVN_GPU_E_UNSUPPORTED,
                            "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
            if (vk_desc_type(e->cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM)
                return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", e->cls);
            uint32_t j = 0;
            for (; j < nm; j++)
                if (m[j].set == e->set && m[j].binding == e->binding) break;
            uint32_t stagebit = 1u << st;
            if (j == nm) {
                if (nm == 64) return fail(err, NVN_GPU_E_UNSUPPORTED, "描述符项超过 64");
                m[nm++] = (struct merged){e->set, e->binding, e->cls, e->count ? e->count : 1, stagebit};
            } else {
                if (m[j].cls != e->cls || m[j].count != (e->count ? e->count : 1))
                    return fail(err, NVN_GPU_E_LAYOUT, "set %u binding %u 的阶段布局冲突", e->set, e->binding);
                m[j].stages |= stagebit;
            }
            if (e->set > maxset) maxset = e->set;
        }
    }
    if (maxset >= 4) return fail(err, NVN_GPU_E_LAYOUT, "set %u 超过四 set 上限", maxset);

    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "管线结构分配失败");
    p->nset = nm ? maxset + 1 : 0;
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < nm; i++) {
            if (m[i].set != set) continue;
            if (nb == 32) { free(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = m[i].binding;
            b[nb].descriptorType = vk_desc_type(m[i].cls);
            b[nb].descriptorCount = m[i].count;
            b[nb].stageFlags = 0;
            for (int st = 0; st < 5; st++)
                if (m[i].stages & (1u << st)) b[nb].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
            nb++;
        }
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = nb;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(g.d, &li, NULL, &p->dsl[set]) != VK_SUCCESS) {
            free(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
    }
    VkPushConstantRange pr[6];
    uint32_t npr = 0;
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh || !sh->push_bytes) continue;
        if ((sh->push_off | sh->push_bytes) & 3) {
            free(p);
            return fail(err, NVN_GPU_E_LAYOUT, "阶段 %d push 范围 [%u,+%u) 未 4 字节对齐", st, sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            free(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "阶段 %d push 范围 [%u,+%u) 超出设备上限 %u",
                        st, sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        uint32_t k;
        for (k = 0; k < npr; k++)
            if (pr[k].offset == sh->push_off && pr[k].size == sh->push_bytes) {
                pr[k].stageFlags |= vk_shader_stage((nvn_gpu_stage)st);
                break;
            }
        if (k == npr)
            pr[npr++] = (VkPushConstantRange){vk_shader_stage((nvn_gpu_stage)st), sh->push_off, sh->push_bytes};
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = npr;
    pli.pPushConstantRanges = npr ? pr : NULL;
    if (vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout) != VK_SUCCESS) {
        free(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreatePipelineLayout 失败");
    }
    p->pass = make_pipeline_pass(d);
    if (!p->pass) {
        vkDestroyPipelineLayout(g.d, p->layout, NULL);
        free(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "管线 render pass 创建失败（附件格式 / 采样数）");
    }

    VkPipelineShaderStageCreateInfo stages[5];
    uint32_t ns = 0;
    for (int st = 0; st < 5; st++) {
        if (!d->st[st].sh) continue;
        memset(&stages[ns], 0, sizeof stages[ns]);
        stages[ns].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[ns].stage = vk_shader_stage((nvn_gpu_stage)st);
        stages[ns].module = d->st[st].sh->mod;
        stages[ns].pName = d->st[st].sh->entry;
        ns++;
    }

    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nvb = 0, nva = 0;
    for (int i = 0; i < 16; i++) {
        if (!d->attr[i].fmt) continue;
        VkFormat f = nvn_vk_format(nvn_vattr_fmt_canon(d->attr[i].fmt));   // RGBX → 同宽 RGB（X 不参与）
        if (f == VK_FORMAT_UNDEFINED) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "顶点属性格式 0x%x 无映射", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        // 必须 CPU 解码的格式（32 位 UI2F/I2F）：本后端没有解码合成 binding，按映射出的整数
        // 格式绑定会把整数位模式当 float 静默读错值——明确报红，不返回成功兜底。
        if (nvn_vattr_must_cpu_decode(d->attr[i].fmt)) {
            nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED,
                                  "顶点属性格式 0x%x 需要 CPU 顶点解码（本后端未实现）", d->attr[i].fmt);
            destroy_pipe_objects(p);
            return bad;
        }
        uint32_t stream = d->attr[i].stream;
        int found = 0;
        for (uint32_t k = 0; k < nvb; k++) if (vb[k].binding == stream) found = 1;
        if (!found) {
            vb[nvb].binding = stream;
            vb[nvb].stride = d->stream[stream].stride;
            vb[nvb].inputRate = d->stream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            nvb++;
        }
        va[nva].location = (uint32_t)i;
        va[nva].binding = stream;
        va[nva].format = f;
        va[nva].offset = d->attr[i].offset;
        nva++;
    }

    VkPipelineVertexInputStateCreateInfo vin;
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = nvb;
    vin.pVertexBindingDescriptions = vb;
    vin.vertexAttributeDescriptionCount = nva;
    vin.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vk_topology(d->prim);
    // 重启只在条带 / 扇上有意义；列表图元打开需要 primitiveTopologyListRestart，这里忽略并注释
    int restart_topology = ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                           ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                           ia.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    ia.primitiveRestartEnable = (d->prim_restart && restart_topology) ? VK_TRUE : VK_FALSE;
    if (ia.topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "图元 %u：本段只实现三角形", d->prim);
        destroy_pipe_objects(p);
        return bad;
    }

    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = d->poly.mode == 0 ? VK_POLYGON_MODE_POINT : d->poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d->poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : d->poly.cull == 2 ? VK_CULL_MODE_BACK_BIT
                 : d->poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    // 正面绕序（契约 v6）：poly.front 是 y 朝上的规范化设备坐标里的绕序。Vulkan 在帧缓冲坐标（y 朝下）里判正面：
    // 视口不翻（原点左下，见 nvn_gpu_draw）时帧缓冲里看到的绕序与它相反；视口翻了（原点左上，负高度）时相同。
    { int ccw_ndc = d->poly.front != 0, ccw_fb = d->origin_ll ? !ccw_ndc : ccw_ndc;
      rs.frontFace = ccw_fb ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; }
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = d->depth_clamp ? VK_TRUE : VK_FALSE;   // 设备特性 depthClamp 是必需项（vk_device.c）
    rs.depthBiasEnable = (d->poly.offset_en & (1u << d->poly.mode)) ? VK_TRUE : VK_FALSE;   // NVN 点 / 线 / 填充偏移位分别为 1 / 2 / 4
    rs.rasterizerDiscardEnable = d->rast_discard ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(d->samples > 1 ? d->samples : 1);

    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (d->dfmt) {
        ds.depthTestEnable = d->ds.dt ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = d->ds.dw ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = vk_depth_func(d->ds.df);
        ds.depthBoundsTestEnable = VK_FALSE;
    }
    if (d->sfmt && d->ds.st) {   // 模板：比较函数与三种操作进管线；读掩码 / 写掩码 / 参考值是动态状态（apply_dyn）。通道没有模板附件时测试恒过，不开
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){vk_stencil_op(d->ds.fail_f), vk_stencil_op(d->ds.zpass_f), vk_stencil_op(d->ds.zfail_f), vk_depth_func(d->ds.sf_f), 0xff, 0xff, 0};
        ds.back = (VkStencilOpState){vk_stencil_op(d->ds.fail_b), vk_stencil_op(d->ds.zpass_b), vk_stencil_op(d->ds.zfail_b), vk_depth_func(d->ds.sf_b), 0xff, 0xff, 0};
    }

    VkLogicOp logic_op = vk_logic_op(d->color.logic_op);
    if (logic_op == VK_LOGIC_OP_MAX_ENUM) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_ARG, "逻辑操作 %u 非法", d->color.logic_op);
        destroy_pipe_objects(p);
        return bad;
    }
    int logic_enabled = d->color.logic_op != NVN_LOGIC_OP_COPY;
    if (logic_enabled && !g.dev->core_features.logicOp) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "逻辑操作 %u：设备未启用 logicOp", d->color.logic_op);
        destroy_pipe_objects(p);
        return bad;
    }
    VkPipelineColorBlendAttachmentState cb[8];
    uint32_t ncb = 0, nslot = 0;
    for (uint32_t i = 0; i < 8; i++) if (d->cfmt[i]) nslot = i + 1;
    for (uint32_t i = 0; i < nslot; i++) {
        memset(&cb[ncb], 0, sizeof cb[ncb]);
        if (!d->cfmt[i]) { ncb++; continue; }   // 空槽：占一个位置，不混合、不写
        // 整数格式的目标从不混合（硬件忽略它的混合状态；Vulkan 上整数附件开混合非法）
        nvn_fmt_info cfi; int is_int = nvn_fmt_info_of(d->cfmt[i], &cfi) && (cfi.kind == NVN_FMT_UINT || cfi.kind == NVN_FMT_SINT);
        if (!logic_enabled && ((d->color.blend_en >> i) & 1u) && !is_int) {
            const blend_st *b = &d->blend[i];
            if (b->adv_mode) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "高级混合（adv_mode）：本段未实现（target %u）", i);
                destroy_pipe_objects(p);
                return bad;
            }
            uint32_t fs[4] = {b->src_rgb, b->dst_rgb, b->src_a, b->dst_a};
            for (int k = 0; k < 4; k++)
                if (fs[k] >= 0x10 && fs[k] <= 0x13 && !g.dev->core_features.dualSrcBlend) {
                    nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "双源混合因子：设备未启用 dualSrcBlend（target %u）", i);
                    destroy_pipe_objects(p);
                    return bad;
                }
            VkBlendFactor sf = vk_blend_factor(b->src_rgb), df = vk_blend_factor(b->dst_rgb);
            VkBlendFactor sa = vk_blend_factor(b->src_a), da = vk_blend_factor(b->dst_a);
            VkBlendOp er = vk_blend_op(b->eq_rgb), ea = vk_blend_op(b->eq_a);
            if (sf == VK_BLEND_FACTOR_MAX_ENUM || df == VK_BLEND_FACTOR_MAX_ENUM || sa == VK_BLEND_FACTOR_MAX_ENUM ||
                da == VK_BLEND_FACTOR_MAX_ENUM || er == VK_BLEND_OP_MAX_ENUM || ea == VK_BLEND_OP_MAX_ENUM) {
                nvn_gpu_rc bad = fail(err, NVN_GPU_E_UNSUPPORTED, "混合因子 / 运算：target %u 未实现", i);
                destroy_pipe_objects(p);
                return bad;
            }
            cb[ncb].blendEnable = VK_TRUE;
            cb[ncb].srcColorBlendFactor = sf; cb[ncb].dstColorBlendFactor = df; cb[ncb].colorBlendOp = er;
            cb[ncb].srcAlphaBlendFactor = sa; cb[ncb].dstAlphaBlendFactor = da; cb[ncb].alphaBlendOp = ea;
        }
        uint32_t mask = (d->chmask.mask >> (i * 4)) & 0xF;   // 0 = 这个目标四个通道都不写（只写深度的绘制），不是「没设」
        cb[ncb].colorWriteMask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                 ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
        ncb++;
    }
    VkPipelineColorBlendStateCreateInfo cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // COPY 保留原混合路径；其余操作取代混合。Vulkan 对 float / sRGB 附件自动跳过逻辑运算。
    cbs.logicOpEnable = logic_enabled ? VK_TRUE : VK_FALSE;
    cbs.logicOp = logic_op;
    cbs.attachmentCount = ncb;
    cbs.pAttachments = cb;

    VkDynamicState dyn[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                             VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dyns;
    memset(&dyns, 0, sizeof dyns);
    dyns.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 7;
    dyns.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = ns;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dyns;
    gp.layout = p->layout;
    gp.renderPass = p->pass;
    gp.subpass = 0;
    const uint64_t pl_t0 = now_ns();   // 驱动编译管线是同步的：超过 3 毫秒就打一行（走路/进新场景时会集中出现）
    if (vkCreateGraphicsPipelines(g.d, g_plc, 1, &gp, NULL, &p->pipe) != VK_SUCCESS) {
        nvn_gpu_rc bad = fail(err, NVN_GPU_E_COMPILE, "vkCreateGraphicsPipelines 失败");
        destroy_pipe_objects(p);
        return bad;
    }
    plc_created();
    { const uint64_t dt = now_ns() - pl_t0;   // 编译耗时：>3 毫秒单独记一行（正常建管线在 0.2–1 毫秒）
      static int slow = -1; if (slow < 0) slow = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      if (slow && dt > 3000000ull) { char b[160]; snprintf(b, sizeof b, "图形管线编译 %.1f 毫秒", (double)dt / 1e6); nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", b); } }
    for (int st = 0; st < 5; st++) {
        struct nvn_gpu_shader_s *sh = d->st[st].sh;
        if (!sh) continue;
        p->stage_nent[st] = sh->nent;
        p->stage_push_off[st] = sh->push_off;
        p->stage_push_bytes[st] = sh->push_bytes;
        if (sh->nent) {
            p->stage_ents[st] = calloc(sh->nent, sizeof *p->stage_ents[st]);
            if (!p->stage_ents[st]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "图形管线阶段布局分配失败");
            }
            memcpy(p->stage_ents[st], sh->ents, (size_t)sh->nent * sizeof *p->stage_ents[st]);
            if (sh->tfmt) {
                p->stage_tfmt[st] = calloc(sh->nent, sizeof *p->stage_tfmt[st]);
                if (!p->stage_tfmt[st]) {
                    destroy_pipe_objects(p);
                    return fail(err, NVN_GPU_E_OOM, "图形管线 texel 格式表分配失败");
                }
                memcpy(p->stage_tfmt[st], sh->tfmt, (size_t)sh->nent * sizeof *p->stage_tfmt[st]);
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    for (int st = 0; st < 5; st++)
        if (d->st[st].sh) d->st[st].sh->refs++; // 管线持有
    pthread_mutex_unlock(&g_mu);
    p->key = *d;
    pthread_mutex_lock(&g_mu);
    p->all_next = g.pipes;
    g.pipes = p;
    p->hnext = g_pipe_ht[hb]; g_pipe_ht[hb] = p;
    pthread_mutex_unlock(&g_mu);
    *out = p;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_cpipe_get(const nvn_gpu_stage_desc *cs, int mode, nvn_gpu_pipe *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    (void)mode;
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!cs || !cs->sh) return fail(err, NVN_GPU_E_ARG, "计算阶段为空");
    struct nvn_gpu_shader_s *sh = cs->sh;
    if (sh->stage != NVN_GPU_SH_CS) return fail(err, NVN_GPU_E_ARG, "阶段不是 CS");
    if (sh->missing_storage_format_feature)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "设备未启用无格式存储图像所需特性");
    struct nvn_gpu_pipe_s *p = calloc(1, sizeof *p);
    if (!p) return fail(err, NVN_GPU_E_OOM, "计算管线结构分配失败");
    uint32_t maxset = 0;
    for (uint32_t i = 0; i < sh->nent; i++) {
        if (sh->ents[i].cls == NVN_GPU_RES_VA) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED,
                        "VA（设备地址）类别：契约未定非 bufferDeviceAddress 的消费方式，不报绿（请契约属主定夺）");
        }
        if (vk_desc_type(sh->ents[i].cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "资源类别 %u：本段未实现", sh->ents[i].cls);
        }
        if (sh->ents[i].set > maxset) maxset = sh->ents[i].set;
    }
    if (sh->nent && maxset >= 4) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_LAYOUT, "set %u 超过四 set 上限", maxset); }
    p->nset = sh->nent ? maxset + 1 : 0;
    for (uint32_t set = 0; set < p->nset; set++) {
        VkDescriptorSetLayoutBinding b[32];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < sh->nent; i++) {
            if (sh->ents[i].set != set) continue;
            if (nb == 32) { destroy_pipe_objects(p); return fail(err, NVN_GPU_E_UNSUPPORTED, "单 set 描述符项超过 32"); }
            memset(&b[nb], 0, sizeof b[nb]);
            b[nb].binding = sh->ents[i].binding;
            b[nb].descriptorType = vk_desc_type(sh->ents[i].cls);
            b[nb].descriptorCount = sh->ents[i].count ? sh->ents[i].count : 1;
            b[nb].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            nb++;
        }
        VkDescriptorSetLayoutCreateInfo li;
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = nb;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(g.d, &li, NULL, &p->dsl[set]) != VK_SUCCESS) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateDescriptorSetLayout 失败（set %u）", set);
        }
    }
    VkPushConstantRange cpr;
    uint32_t ncpr = 0;
    if (sh->push_bytes) {
        if ((sh->push_off | sh->push_bytes) & 3) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_LAYOUT, "CS push 范围 [%u,+%u) 未 4 字节对齐", sh->push_off, sh->push_bytes);
        }
        if (sh->push_off > g.caps.max_push_bytes || sh->push_bytes > g.caps.max_push_bytes - sh->push_off) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "CS push 范围 [%u,+%u) 超出设备上限 %u",
                        sh->push_off, sh->push_bytes, g.caps.max_push_bytes);
        }
        cpr = (VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, sh->push_off, sh->push_bytes};
        ncpr = 1;
    }
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = p->nset;
    pli.pSetLayouts = p->nset ? p->dsl : NULL;
    pli.pushConstantRangeCount = ncpr;
    pli.pPushConstantRanges = ncpr ? &cpr : NULL;
    if (vkCreatePipelineLayout(g.d, &pli, NULL, &p->layout) != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreatePipelineLayout 失败");
    }
    VkComputePipelineCreateInfo cp;
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = sh->mod;
    cp.stage.pName = sh->entry;
    cp.layout = p->layout;
    const uint64_t pc_t0 = now_ns();   // 同图形管线：超过 3 毫秒打一行，用来对长帧
    if (vkCreateComputePipelines(g.d, g_plc, 1, &cp, NULL, &p->pipe) != VK_SUCCESS) {
        destroy_pipe_objects(p);
        return fail(err, NVN_GPU_E_COMPILE, "vkCreateComputePipelines 失败");
    }
    plc_created();
    { const uint64_t dt = now_ns() - pc_t0;
      static int slowc = -1; if (slowc < 0) slowc = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      if (slowc && dt > 3000000ull) { char b[160]; snprintf(b, sizeof b, "计算管线编译 %.1f 毫秒", (double)dt / 1e6); nvn_plat_log(NVN_PLAT_LOG_INFO, "pipe", b); } }
    p->stage_nent[NVN_GPU_SH_CS] = sh->nent;
    p->stage_push_off[NVN_GPU_SH_CS] = sh->push_off;
    p->stage_push_bytes[NVN_GPU_SH_CS] = sh->push_bytes;
    if (sh->nent) {
        p->stage_ents[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_ents[NVN_GPU_SH_CS]);
        if (!p->stage_ents[NVN_GPU_SH_CS]) {
            destroy_pipe_objects(p);
            return fail(err, NVN_GPU_E_OOM, "计算管线阶段布局分配失败");
        }
        memcpy(p->stage_ents[NVN_GPU_SH_CS], sh->ents, (size_t)sh->nent * sizeof *sh->ents);
        if (sh->tfmt) {
            p->stage_tfmt[NVN_GPU_SH_CS] = calloc(sh->nent, sizeof *p->stage_tfmt[NVN_GPU_SH_CS]);
            if (!p->stage_tfmt[NVN_GPU_SH_CS]) {
                destroy_pipe_objects(p);
                return fail(err, NVN_GPU_E_OOM, "计算管线 texel 格式表分配失败");
            }
            memcpy(p->stage_tfmt[NVN_GPU_SH_CS], sh->tfmt, (size_t)sh->nent * sizeof *sh->tfmt);
        }
    }
    pthread_mutex_lock(&g_mu);
    sh->refs++;
    p->all_next = g.pipes;
    g.pipes = p;
    pthread_mutex_unlock(&g_mu);
    *out = p;
    return rc(NVN_GPU_OK);
}

// 附件形态的视图：一级一层的 2D 视图，深度模板格式两个面都在（对照 nvn_mtl.m attach @4928：附件 = 基底 + 视图设了才取的
// level / slice，没设就是首级首层）。采样用的视图可以是多级 / 多层 / 立方体 / 只取深度面，都不能直接挂到帧缓冲上：
// 第一次当附件时另建一个，随视图销毁。建不出来回空。
static VkImageView view_attachment(struct nvn_gpu_view_s *v) {
    struct nvn_gpu_tex_s *t = v->tex;
    if (!v->swizzled && v->levels == 1 && v->layers == 1 && v->type == VK_IMAGE_VIEW_TYPE_2D && v->aspect == t->aspect) return v->view;
    pthread_mutex_lock(&g_mu);
    int image3d = t->target == NVN_TEXTURE_TARGET_3D && t->depth > 1;
    if (!v->att && !(image3d && !t->array_compatible)) {
        VkImageViewCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = t->image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = v->format ? v->format : t->format;
        ci.subresourceRange = (VkImageSubresourceRange){t->aspect, v->base_level, 1, image3d ? v->att_layer : v->base_layer, 1};
        OC(OC_VIEW);
        { LIFE_T0; if (vkCreateImageView(g.d, &ci, NULL, &v->att) != VK_SUCCESS) v->att = VK_NULL_HANDLE; LIFE_ADD(4); }
    }
    VkImageView r = v->att;
    pthread_mutex_unlock(&g_mu);
    return r;
}

// 图像形态的视图：存储图像视图必须用恒等通道映射（Vulkan 规定 image 视图不参与通道映射），而采样视图可能带着
// 通道映射（游戏设的，或没有 alpha 的格式把 a 钉成 ONE）。第一次当存储图像时另建一个，随视图销毁。
static VkImageView view_image_form(struct nvn_gpu_view_s *v) {
    if (!v->swizzled) return v->view;
    pthread_mutex_lock(&g_mu);
    if (!v->img) {
        VkImageViewCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = v->tex->image;
        ci.viewType = v->type;
        ci.format = v->format;
        ci.subresourceRange = (VkImageSubresourceRange){v->aspect, v->base_level, v->levels, v->base_layer, v->layers};
        OC(OC_VIEW);
        { LIFE_T0; if (vkCreateImageView(g.d, &ci, NULL, &v->img) != VK_SUCCESS) v->img = VK_NULL_HANDLE; LIFE_ADD(4); }
    }
    VkImageView r = v->img;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static pthread_mutex_t g_pc_mu = PTHREAD_MUTEX_INITIALIZER;
// 开关每 128 个通道重读一次（真机上同一局里来回切着比）；一个通道从头到尾用开头读到的那个值（g_pc_use，只在回放线程上动）
static int g_pc_use = 1;
static int pc_on(void) { static int on = -1; static uint32_t n; if (on < 0 || !(++n & 127)) on = nvn_plat_flag("nvk_pass_cache", 1) != 0; return on; }
struct pc_rp { uint32_t natt, ncol; int has_depth; VkAttachmentDescription att[9]; VkAttachmentReference col[8], dref; VkRenderPass rp; };
static struct pc_rp *g_pc_rp; static uint32_t g_pc_nrp, g_pc_rpcap;
// 帧缓冲缓存：真机上「开通道内部」的帧缓冲一段读到 7.7–12.8 微秒/通道、未命中 25–34 个/帧
// （未命中就只能 vkCreateFramebuffer，7–12 微秒一个）。表从 2048 加到 8192、探测 24 加到 32，专门压未命中率；
// 反查表（pc_fb_note_desc / pc_drop_view_fast）在容量变大后仍然是按视图定点拆，不做全表扫描。
enum { PC_FB_N = 8192, PC_FB_PROBE = 32 };
struct pc_fb { VkRenderPass rp; uint32_t natt, w, h, layers; VkImageView v[9]; VkFramebuffer fb; uint32_t gen; int tomb; };
static struct pc_fb g_pc_fb[PC_FB_N];
static VkRenderPass pc_pass(const VkRenderPassCreateInfo *ci, const VkAttachmentDescription *att, uint32_t natt, const VkAttachmentReference *col, uint32_t ncol, const VkAttachmentReference *dref) {
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!g_pc_use || natt > 9 || ncol > 8) { OC(OC_RP); return vkCreateRenderPass(g.d, ci, NULL, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE; }
    struct pc_rp k; memset(&k, 0, sizeof k); k.natt = natt; k.ncol = ncol; k.has_depth = dref != NULL;
    memcpy(k.att, att, natt * sizeof *att); memcpy(k.col, col, ncol * sizeof *col); if (dref) k.dref = *dref;
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < g_pc_nrp; i++) if (!memcmp(&g_pc_rp[i], &k, offsetof(struct pc_rp, rp))) { pass = g_pc_rp[i].rp; pthread_mutex_unlock(&g_pc_mu); return pass; }
    OC(OC_RP);
    if (vkCreateRenderPass(g.d, ci, NULL, &pass) != VK_SUCCESS) { pthread_mutex_unlock(&g_pc_mu); return VK_NULL_HANDLE; }
    if (g_pc_nrp == g_pc_rpcap) { uint32_t cap = g_pc_rpcap ? g_pc_rpcap * 2 : 64; struct pc_rp *n = realloc(g_pc_rp, cap * sizeof *n); if (n) { g_pc_rp = n; g_pc_rpcap = cap; } }
    if (g_pc_nrp < g_pc_rpcap) { k.rp = pass; g_pc_rp[g_pc_nrp++] = k; }   // 记不下（内存不够）：这个对象留到会话结束也没人销毁；只在分配失败时发生
    pthread_mutex_unlock(&g_pc_mu);
    return pass;
}
static uint32_t g_fb_gen;   // 帧缓冲缓存槽的代：槽被复用时，视图反查表里记的旧引用会因代不同而被忽略
static uint64_t g_fb_hit, g_fb_miss, g_fb_create_ns;   // 读数（nvk_pass_prof）：帧缓冲命中 / 未命中 / 新建耗时
static VkFramebuffer pc_fb(const VkFramebufferCreateInfo *fi, int *cached, int *slot_out, uint32_t *gen_out) {
    VkFramebuffer fb = VK_NULL_HANDLE; *cached = 0; if (slot_out) *slot_out = -1; if (gen_out) *gen_out = 0;
    if (!g_pc_use || fi->attachmentCount > 9) { OC(OC_FB); return vkCreateFramebuffer(g.d, fi, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE; }
    if (!g_pc_use || fi->attachmentCount > 9) { OC(OC_FB); return vkCreateFramebuffer(g.d, fi, NULL, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE; }
    uint64_t h = (uint64_t)(uintptr_t)fi->renderPass * 0x9E3779B97F4A7C15ull ^ ((uint64_t)fi->width << 40 | (uint64_t)fi->height << 16 | fi->layers);
    for (uint32_t i = 0; i < fi->attachmentCount; i++) h = (h ^ (uint64_t)(uintptr_t)fi->pAttachments[i]) * 0x100000001B3ull;
    pthread_mutex_lock(&g_pc_mu);
    int slot = -1;
    for (uint32_t p = 0; p < PC_FB_PROBE; p++) {
        struct pc_fb *e = &g_pc_fb[(h + p) % PC_FB_N];
        if (!e->fb) { if (slot < 0) slot = (int)((h + p) % PC_FB_N); if (!e->tomb) break; continue; }
        if (e->rp == fi->renderPass && e->natt == fi->attachmentCount && e->w == fi->width && e->h == fi->height && e->layers == fi->layers &&
            !memcmp(e->v, fi->pAttachments, fi->attachmentCount * sizeof(VkImageView))) { fb = e->fb; *cached = 1; __atomic_fetch_add(&g_fb_hit, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_pc_mu); return fb; }
    }
    OC(OC_FB);
    __atomic_fetch_add(&g_fb_miss, 1, __ATOMIC_RELAXED);
    { struct timespec f0; clock_gettime(CLOCK_MONOTONIC, &f0); LIFE_T0; VkResult fr_ = vkCreateFramebuffer(g.d, fi, NULL, &fb); LIFE_ADD(3); if (fr_ != VK_SUCCESS) { pthread_mutex_unlock(&g_pc_mu); return VK_NULL_HANDLE; }
      struct timespec f1; clock_gettime(CLOCK_MONOTONIC, &f1);
      __atomic_fetch_add(&g_fb_create_ns, (uint64_t)(f1.tv_sec - f0.tv_sec) * 1000000000ull + (uint64_t)f1.tv_nsec - (uint64_t)f0.tv_nsec, __ATOMIC_RELAXED); }
    if (slot >= 0) { struct pc_fb *e = &g_pc_fb[slot]; memset(e, 0, sizeof *e); e->rp = fi->renderPass; e->natt = fi->attachmentCount; e->w = fi->width; e->h = fi->height; e->layers = fi->layers;
        memcpy(e->v, fi->pAttachments, fi->attachmentCount * sizeof(VkImageView)); e->fb = fb; e->gen = ++g_fb_gen; *cached = 1;
        if (slot_out) { *slot_out = slot; }
        if (gen_out) { *gen_out = e->gen; } }
    pthread_mutex_unlock(&g_pc_mu);
    return fb;
}
static void pc_drop_view(VkImageView v) {
    if (!v) return;
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < PC_FB_N; i++) { struct pc_fb *e = &g_pc_fb[i]; if (!e->fb) continue;
        for (uint32_t k = 0; k < e->natt; k++) if (e->v[k] == v) { vkDestroyFramebuffer(g.d, e->fb, NULL); memset(e, 0, sizeof *e); e->tomb = 1; break; } }
    pthread_mutex_unlock(&g_pc_mu);
}
// 视图销毁：先按反查表只拆它当过附件的那些帧缓冲（真机上一帧 30 个视图销毁，全表扫会和开通道抢同一把锁）。
// 反查表溢出过、或表里的项都对不上（槽被复用过、拆过了）才退回全表扫。
static void pc_drop_view_fast(struct nvn_gpu_view_s *view, VkImageView v) {
    if (!v) return;
    if (!view || view->fbs_overflow || !view->nfbs) { pc_drop_view(v); return; }
    pthread_mutex_lock(&g_pc_mu);
    for (uint8_t i = 0; i < view->nfbs; i++) {
        uint16_t idx = view->fbs[i].idx; if (idx >= PC_FB_N) continue;
        struct pc_fb *e = &g_pc_fb[idx];
        if (!e->fb || e->gen != view->fbs[i].gen || e->fb != view->fbs[i].fb) continue;
        vkDestroyFramebuffer(g.d, e->fb, NULL); memset(e, 0, sizeof *e); e->tomb = 1;
    }
    pthread_mutex_unlock(&g_pc_mu);
    view->nfbs = 0; view->fbs_overflow = 0;
}
// 记录「这个视图当过这个帧缓冲的附件」（反查表满 6 项就不再记，销毁时退回全表扫）
static void pc_fb_note(struct nvn_gpu_view_s *view, int slot, uint32_t gen, VkFramebuffer fb) {
    if (!view || slot < 0 || !gen || !fb) return;
    if (view->nfbs >= 6) { view->fbs_overflow = 1; return; }
    view->fbs[view->nfbs].idx = (uint16_t)slot; view->fbs[view->nfbs].gen = gen; view->fbs[view->nfbs].fb = fb; view->nfbs++;
}
static void pc_fb_note_desc(const nvn_gpu_pass_desc *d, int slot, uint32_t gen, VkFramebuffer fb) {
    if (!d) return;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) if (d->color[i].view) pc_fb_note(d->color[i].view, slot, gen, fb);
    if (d->depth.view) pc_fb_note(d->depth.view, slot, gen, fb);
    if (d->stencil.view && d->stencil.view != d->depth.view) pc_fb_note(d->stencil.view, slot, gen, fb);
}
static void pc_destroy_all(void) {
    pthread_mutex_lock(&g_pc_mu);
    for (uint32_t i = 0; i < PC_FB_N; i++) if (g_pc_fb[i].fb) vkDestroyFramebuffer(g.d, g_pc_fb[i].fb, NULL);
    memset(g_pc_fb, 0, sizeof g_pc_fb);
    for (uint32_t i = 0; i < g_pc_nrp; i++) vkDestroyRenderPass(g.d, g_pc_rp[i].rp, NULL);
    free(g_pc_rp); g_pc_rp = NULL; g_pc_nrp = g_pc_rpcap = 0;
    pthread_mutex_unlock(&g_pc_mu);
}
// 渲染通道的附件描述：布局按「这张图这一段用什么布局」定（见「图像布局」）。
// att_tex / att_lay 回填各附件的贴图与这一段用的布局；att_tex 可能重复（同一张图的不同级 / 层挂在不同槽）。
static VkRenderPass make_pass(const nvn_gpu_pass_desc *d, VkImageView fb_views[9], VkAttachmentDescription att[9],
                              VkAttachmentReference col[8], VkAttachmentReference *dref, uint32_t *natt_out, int *has_depth,
                              struct nvn_gpu_tex_s *att_tex[9], VkImageLayout att_lay[9], int opt) {
    int is_depth[9];
    uint32_t natt = 0, a = 0, ncol = 0;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {   // 先归拢：真正挂上的颜色附件（按槽序），空槽不算附件
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (!v) continue;
        att_tex[natt] = v->tex; is_depth[natt] = 0; natt++;
    }
    if (d->depth.view || d->stencil.view) {   // 深度模板只算一个附件（描述里没给视图的那一面不关心）
        att_tex[natt] = (d->depth.view ? d->depth.view : d->stencil.view)->tex; is_depth[natt] = 1; natt++;
    }
    for (uint32_t i = 0; i < natt; i++) att_lay[i] = pass_att_layout(d, att_tex[i], is_depth[i], opt);
    for (uint32_t i = 0; i < natt; i++)   // 同一张图的几个级 / 层挂在不同槽：布局必须一致，有一个要走 GENERAL 就都走 GENERAL
        for (uint32_t j = i + 1; j < natt; j++)
            if (att_tex[i] == att_tex[j] && att_lay[i] != att_lay[j]) att_lay[i] = att_lay[j] = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference stencil_ref;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        struct nvn_gpu_view_s *v = d->color[i].view;
        if (!v) { col[ncol].attachment = VK_ATTACHMENT_UNUSED; col[ncol].layout = VK_IMAGE_LAYOUT_UNDEFINED; ncol++; continue; }   // 颜色槽里的空位
        struct nvn_gpu_tex_s *t = v->tex;
        memset(&att[a], 0, sizeof att[a]);
        att[a].format = v->format ? v->format : t->format;   // 按视图的格式声明（对照 nvn_mtl.m attach @4883：视图换了格式就按那个格式的视图挂上去，管线格式跟着附件）
        att[a].samples = (VkSampleCountFlagBits)t->samples;
        switch (d->color[i].load) {
        case NVN_GPU_LOAD_CLEAR: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
        case NVN_GPU_LOAD_DONTCARE: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
        default: att[a].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; break;
        }
        att[a].initialLayout = att_lay[a];   // 附件在通道里用的布局：缺省 GENERAL；开 nvk_opt_layout 用附件专用布局
        att[a].storeOp = d->color[i].store == NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        att[a].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[a].finalLayout = att_lay[a];
        col[ncol].attachment = a;
        col[ncol].layout = att_lay[a];
        fb_views[a] = view_attachment(v);
        if (!fb_views[a]) return VK_NULL_HANDLE;
        ncol++; a++;
    }
    *has_depth = 0;
    if (d->depth.view || d->stencil.view) {
        // 深度与模板是同一个附件的两个面，各按自己的 load / store 走；描述里没给视图的那一面不关心。
        // 只要有一面要保留原内容（LOAD），附件就必须从它现在的布局进来：从 UNDEFINED 进来等于两面都丢。
        struct nvn_gpu_view_s *v = d->depth.view ? d->depth.view : d->stencil.view;
        struct nvn_gpu_tex_s *t = v->tex;
        static const VkAttachmentLoadOp k_load[3] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE};
        memset(&att[a], 0, sizeof att[a]);
        att[a].format = t->format;
        att[a].samples = (VkSampleCountFlagBits)t->samples;
        att[a].loadOp = d->depth.view ? k_load[d->depth.load < 3 ? d->depth.load : 2] : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].stencilLoadOp = d->stencil.view ? k_load[d->stencil.load < 3 ? d->stencil.load : 2] : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[a].initialLayout = att_lay[a];
        att[a].storeOp = d->depth.view && d->depth.store != NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[a].stencilStoreOp = d->stencil.view && d->stencil.store != NVN_GPU_STORE_DONTCARE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[a].finalLayout = att_lay[a];
        dref->attachment = a;
        dref->layout = att_lay[a];
        fb_views[a] = view_attachment(v);
        if (!fb_views[a]) return VK_NULL_HANDLE;
        *has_depth = 1;
        a++;
    }
    (void)stencil_ref;
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = ncol;
    sp.pColorAttachments = ncol ? col : NULL;
    sp.pDepthStencilAttachment = *has_depth ? dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo rp;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = a;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    *natt_out = a;
    return pc_pass(&rp, att, a, col, ncol, *has_depth ? dref : NULL);
}

// 开通道用的 VkRenderPassBeginInfo：清除值按附件下标排（空槽不占附件）、渲染区域按 desc 收缩并裁到帧缓冲里。
static void pass_begin_info(const nvn_gpu_pass_desc *d, VkRenderPass pass, VkFramebuffer fb, uint32_t natt, int has_depth,
                            VkRenderPassBeginInfo *bi, VkClearValue clear[9]) {
    memset(clear, 0, 9 * sizeof clear[0]);
    for (uint32_t i = 0, k = 0; i < d->ncolor && i < 8; i++) {   // 清除值按附件下标排：空槽不占附件
        if (!d->color[i].view) continue;
        memcpy(&clear[k++].color, &d->color[i].clear, sizeof clear[0].color);
    }
    if (has_depth) { clear[natt - 1].depthStencil.depth = d->clear_depth; clear[natt - 1].depthStencil.stencil = d->clear_stencil; }
    memset(bi, 0, sizeof *bi);
    bi->sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi->renderPass = pass;
    bi->framebuffer = fb;
    bi->renderArea.extent = (VkExtent2D){d->w, d->h};
    // 渲染区域：核心按这一遍会碰到的像素给出（0 宽高 = 整个目标）。附件只在区域内载入 / 保存，区域外保持原内容（Vulkan renderArea 语义），
    // 小目标小遍就不必按整张附件算载入与保存。裁到帧缓冲范围里，越界的区域是无效参数。
    if (d->area_w && d->area_h) {
        int32_t rx = d->area_x, ry = d->area_y; uint32_t rw = d->area_w, rh = d->area_h;
        if (rx < 0) { rw = (uint32_t)((int32_t)rw + rx); rx = 0; }
        if (ry < 0) { rh = (uint32_t)((int32_t)rh + ry); ry = 0; }
        if ((uint32_t)rx > d->w) rw = 0; else if ((uint32_t)rx + rw > d->w) rw = d->w - (uint32_t)rx;
        if ((uint32_t)ry > d->h) rh = 0; else if ((uint32_t)ry + rh > d->h) rh = d->h - (uint32_t)ry;
        if (rw && rh) { bi->renderArea.offset = (VkOffset2D){rx, ry}; bi->renderArea.extent = (VkExtent2D){rw, rh}; }
    }
    bi->clearValueCount = natt;
    bi->pClearValues = clear;
}

// 记在段上的「段做完要销毁」的渲染通道 / 帧缓冲（没进缓存的才记）
static int pass_rec_add(struct nvn_gpu_cmd_s *c, VkRenderPass rp, VkFramebuffer fb) {
    if (!rp && !fb) return 1;
    if (c->npasses == c->passcap) {
        uint32_t cap = c->passcap ? c->passcap * 2 : 8;
        struct pass_rec *np = realloc(c->passes, cap * sizeof *np);
        if (!np) return 0;
        c->passes = np; c->passcap = cap;
    }
    c->passes[c->npasses++] = (struct pass_rec){rp, fb};
    return 1;
}

// 这张图是不是当前通道的附件（同一张图的任意级 / 层都算：Vulkan 的布局是整张图的）
static int pass_attached(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    if (!c->in_pass) return 0;
    for (uint32_t i = 0; i < c->pass_natt; i++) if (c->pass_tex[i] == t) return 1;
    return 0;
}

static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to);   // 下面「贴图 / 视图 / 采样器」一节

// 反馈环 / 通道内要改布局：当前通道的附件又要被采样（这张图这一段退回 GENERAL），或存储图像要 GENERAL 而它现在是只读布局。
// Vulkan 通道内不许改布局，所以在通道边界上重开：结束当前通道（附件内容按 finalLayout 留在显存里），
// 把要改的图在通道外迁好，再按 LOAD / STORE 重开同一组附件。只在 nvk_opt_layout=1 时才会走到；
// 开通道前能迁的都在 pass_begin / pass_flush_pending 里迁完了，这里只兜通道里新出现的绑定。
struct reopen_fix { struct nvn_gpu_tex_s *t; VkImageLayout want; };

// 进通道之前：本段里还停在「附件布局」、又不是本通道附件的图，一律先转成只读采样布局（在通道外转）。
// 这样通道里采样任何图都不用再迁布局，也顺带拿到 SHADER_READ_ONLY 的语义；下一段还要当附件时，
// 通道开头再按附件布局迁回来；提交前统一转回 GENERAL（lay_rest_at_commit）。
static void pass_flush_pending(struct nvn_gpu_cmd_s *c) {
    if (!c->opt) return;
    for (uint32_t i = 0; i < c->nlay; i++) {
        struct nvn_gpu_tex_s *t = c->lay[i].t;
        int attached = 0;
        for (uint32_t k = 0; k < c->pass_natt; k++) if (c->pass_tex[k] == t) { attached = 1; break; }
        if (attached) continue;
        if (c->lay[i].cur == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) image_barrier(c, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else if (c->lay[i].cur == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) image_barrier(c, t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    }
}

static int pass_reopen(struct nvn_gpu_cmd_s *c, const struct reopen_fix *fx, uint32_t nfx) {
    if (!c->in_pass || !c->opt || c->q_active >= 0) return 0;   // 查询不能跨通道：声明了 occlusion 的通道开通道时附件就全 GENERAL，正常到不了这里
    nvn_gpu_pass_desc d = c->pass_desc;
    for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) { d.color[i].load = NVN_GPU_LOAD_LOAD; d.color[i].store = NVN_GPU_STORE_STORE; }
    if (d.depth.view) { d.depth.load = NVN_GPU_LOAD_LOAD; d.depth.store = NVN_GPU_STORE_STORE; }
    if (d.stencil.view) { d.stencil.load = NVN_GPU_LOAD_LOAD; d.stencil.store = NVN_GPU_STORE_STORE; }
    if (c->tsq && c->ntsp < TS_PASS_MAX) { vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 3 + 2 * c->ntsp); c->ntsp++; }
    vkCmdEndRenderPass(c->cb);
    c->in_pass = 0;
    c->pass_occl = 0;
    VkImageView fb_views[9];
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    struct nvn_gpu_tex_s *att_tex[9]; VkImageLayout att_lay[9];
    uint32_t natt = 0;
    int has_depth = 0;
    g_pc_use = pc_on();
    VkRenderPass pass = make_pass(&d, fb_views, att, col, &dref, &natt, &has_depth, att_tex, att_lay, c->opt);
    if (!pass) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "通道重开：渲染通道创建失败"); return 0; }
    for (uint32_t i = 0; i < natt; i++) {   // 新布局（fb_sample 置位的附件变成 GENERAL），要在开新通道之前转完
        image_barrier(c, att_tex[i], att_lay[i]);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return 0; }
    }
    for (uint32_t i = 0; i < nfx; i++) {   // 通道里新出现、要换布局的图：也在通道外迁
        image_barrier(c, fx[i].t, fx[i].want);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return 0; }
    }
    VkFramebufferCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = pass;
    fi.attachmentCount = natt;
    fi.pAttachments = fb_views;
    fi.width = d.w;
    fi.height = d.h;
    fi.layers = d.layers ? d.layers : 1;
    int fb_cached = 0, fbsl = -1; uint32_t fbgen = 0;
    VkFramebuffer fb = pc_fb(&fi, &fb_cached, &fbsl, &fbgen);
    if (!fb) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); set_sticky(c, NVN_GPU_E_UNSUPPORTED, "通道重开：vkCreateFramebuffer 失败"); return 0; }
    pc_fb_note_desc(&d, fbsl, fbgen, fb);
    VkRenderPassBeginInfo bi;
    VkClearValue clear[9];
    pass_begin_info(&d, pass, fb, natt, has_depth, &bi, clear);
    if (c->tsq && c->ntsp < TS_PASS_MAX) {
        c->tsp[c->ntsp].w = (uint16_t)d.w; c->tsp[c->ntsp].h = (uint16_t)d.h; c->tsp[c->ntsp].draws = 0;
        c->tsp[c->ntsp].ncol = (uint8_t)(has_depth ? natt - 1 : natt); c->tsp[c->ntsp].ds = (uint8_t)has_depth;
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
    vkCmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->in_pass = 1;   // 通道的其它状态（尺寸 / 格式 / 颜色槽 / 层数）与原来一致，不重设
    c->pass_occl = d.occlusion;
    c->pass_natt = natt;
    for (uint32_t i = 0; i < natt; i++) c->pass_tex[i] = att_tex[i];
    if (!pass_rec_add(c, g_pc_use ? VK_NULL_HANDLE : pass, fb_cached ? VK_NULL_HANDLE : fb)) { set_sticky(c, NVN_GPU_E_OOM, "通道列表分配失败"); return 0; }
    return 1;
}

// 诊断读数（开关 debug.s2a.nvk_pass_prof）：开通道内部各步的耗时（纳秒累计 / 次数，每 512 个通道打一行）。
// 真机上一帧 112 个通道，「后端开通道」占 2.5 毫秒（每次 22 微秒），要看清是渲染通道查表、帧缓冲查表还是记账。
static uint64_t g_pbp_ns[6], g_pbp_n;
static void pass_prof(int i, uint64_t t0) {
    static int on = -1; static unsigned k;
    if (on < 0 || !(++k & 255)) on = nvn_plat_flag("nvk_pass_prof", 0) != 0;
    if (!on) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (i >= 0) g_pbp_ns[i] += now - t0;
    else if (++g_pbp_n >= 512) {
        char pl[300];
        snprintf(pl, sizeof pl, "开通道内部（每通道微秒）：渲染通道 %.2f / 附件迁移 %.2f / 帧缓冲 %.2f（命中 %llu / 未命中 %llu，未命中里新建共 %.0f 微秒）/ 开通道 %.2f / 引用与记账 %.2f（%llu 个，已缓存的渲染通道 %u 个）",
                 (double)g_pbp_ns[0] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[1] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[2] / 1e3 / (double)g_pbp_n,
                 (unsigned long long)__atomic_exchange_n(&g_fb_hit, 0, __ATOMIC_RELAXED), (unsigned long long)__atomic_exchange_n(&g_fb_miss, 0, __ATOMIC_RELAXED),
                 (double)__atomic_exchange_n(&g_fb_create_ns, 0, __ATOMIC_RELAXED) / 1e3,
                 (double)g_pbp_ns[3] / 1e3 / (double)g_pbp_n, (double)g_pbp_ns[4] / 1e3 / (double)g_pbp_n, (unsigned long long)g_pbp_n, g_pc_nrp);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass", pl);
        memset(g_pbp_ns, 0, sizeof g_pbp_ns); g_pbp_n = 0;
    }
}
void nvn_gpu_pass_begin(nvn_gpu_cmd ch, const nvn_gpu_pass_desc *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_begin 时已在通道里"); return; }
    struct timespec pbp_ts; clock_gettime(CLOCK_MONOTONIC, &pbp_ts);
    uint64_t pbp_t = (uint64_t)pbp_ts.tv_sec * 1000000000ull + (uint64_t)pbp_ts.tv_nsec;
#define PBP_STEP(i) do { struct timespec z_; clock_gettime(CLOCK_MONOTONIC, &z_); uint64_t n_ = (uint64_t)z_.tv_sec * 1000000000ull + (uint64_t)z_.tv_nsec; pass_prof(i, pbp_t); pbp_t = n_; } while (0)
    es_reset(c);   // 冗余状态消除：新通道先保守地全部重发（#708）
    if (!d || (!d->ncolor && !d->depth.view && !d->stencil.view) || !d->w || !d->h) { set_sticky(c, NVN_GPU_E_ARG, "pass_begin 参数非法（没有任何附件，或尺寸为 0）"); return; }
    if (d->depth.view && d->stencil.view && d->depth.view != d->stencil.view) { set_sticky(c, NVN_GPU_E_ARG, "pass_begin 的深度与模板必须是同一个视图"); return; }
    VkImageView fb_views[9];
    VkAttachmentDescription att[9];
    VkAttachmentReference col[8];
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    struct nvn_gpu_tex_s *att_tex[9]; VkImageLayout att_lay[9];
    uint32_t natt = 0;
    int has_depth = 0;
    g_pc_use = pc_on();
    VkRenderPass pass = make_pass(d, fb_views, att, col, &dref, &natt, &has_depth, att_tex, att_lay, c->opt);
    PBP_STEP(0);
    if (!pass) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "渲染通道创建失败（附件格式 / 采样数）"); return; }
    // 附件先从现在的布局迁到这一段要用的布局：必须在 vkCmdBeginRenderPass 之前，通道内不许再迁
    for (uint32_t i = 0; i < natt; i++) {
        image_barrier(c, att_tex[i], att_lay[i]);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return; }
    }
    c->pass_natt = natt;
    for (uint32_t i = 0; i < natt; i++) c->pass_tex[i] = att_tex[i];
    pass_flush_pending(c);   // 本段里还停在附件布局、又不是本通道附件的图：先在通道外转成只读采样布局
    PBP_STEP(1);
    if (!rc_is(c->sticky, NVN_GPU_OK)) { if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL); return; }
    VkFramebufferCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = pass;
    fi.attachmentCount = natt;
    fi.pAttachments = fb_views;
    fi.width = d->w;
    fi.height = d->h;
    fi.layers = d->layers ? d->layers : 1;
    int fb_cached = 0, fbsl = -1; uint32_t fbgen = 0;
    VkFramebuffer fb = pc_fb(&fi, &fb_cached, &fbsl, &fbgen);
    PBP_STEP(2);
    if (!fb) {
        if (!g_pc_use) vkDestroyRenderPass(g.d, pass, NULL);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "vkCreateFramebuffer 失败");
        return;
    }
    VkRenderPassBeginInfo bi;
    VkClearValue clear[9];
    pass_begin_info(d, pass, fb, natt, has_depth, &bi, clear);
    if (c->tsq && c->ntsp < TS_PASS_MAX) {
        c->tsp[c->ntsp].w = (uint16_t)d->w; c->tsp[c->ntsp].h = (uint16_t)d->h; c->tsp[c->ntsp].draws = 0; c->tsp[c->ntsp].ncol = (uint8_t)(has_depth ? natt - 1 : natt); c->tsp[c->ntsp].ds = (uint8_t)has_depth;
        vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 2 + 2 * c->ntsp);
    }
    vkCmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    PBP_STEP(3);
    c->in_pass = 1;
    c->pass_occl = d->occlusion;
    c->pass_w = d->w;
    c->pass_h = d->h;
    c->pass_ncolor = d->ncolor < 8 ? d->ncolor : 8;
    c->pass_color_mask = 0;
    memset(c->pass_cfmt, 0, sizeof c->pass_cfmt); c->pass_dsfmt = 0; c->pass_samples = 1;
    for (uint32_t i = 0; i < c->pass_ncolor; i++) if (d->color[i].view) {
        c->pass_color_mask |= 1u << i; c->pass_cfmt[i] = d->color[i].view->nvn_format; c->pass_samples = d->color[i].view->tex->samples; }
    if (d->depth.view || d->stencil.view) { struct nvn_gpu_tex_s *dt = (d->depth.view ? d->depth.view : d->stencil.view)->tex; c->pass_dsfmt = dt->nvn_format; c->pass_samples = dt->samples; }
    c->pass_layers = fi.layers;
    c->pass_ds_aspect = (d->depth.view ? d->depth.view->tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (d->stencil.view ? d->stencil.view->tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    c->pass_desc = *d;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) {
        if (!d->color[i].view) continue;
        cmd_ref(c, d->color[i].view, REF_VIEW);   // 附件 view（连带贴图）保活到本段完成
    }
    if (d->depth.view || d->stencil.view) {
        struct nvn_gpu_view_s *dsv = d->depth.view ? d->depth.view : d->stencil.view;
        cmd_ref(c, dsv, REF_VIEW);
    }
    pc_fb_note_desc(d, fbsl, fbgen, fb);
    // 记在段上的只有「段做完要销毁」的：没进缓存的帧缓冲、缓存关着时的渲染通道对象
    PBP_STEP(4);
    pass_prof(-1, 0);
    if (!pass_rec_add(c, g_pc_use ? VK_NULL_HANDLE : pass, fb_cached ? VK_NULL_HANDLE : fb)) { set_sticky(c, NVN_GPU_E_OOM, "通道列表分配失败"); return; }
#undef PBP_STEP
}

void nvn_gpu_pass_end(nvn_gpu_cmd ch) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "pass_end 时不在通道里"); return; }
    if (c->q_active >= 0) { vkCmdEndQuery(c->cb, c->qpool, (uint32_t)c->q_active); c->q_active = -1; }   // 查询不跨通道
    c->pass_occl = 0;
    vkCmdEndRenderPass(c->cb);
    c->in_pass = 0;
    c->pass_natt = 0;
    if (c->tsq && c->ntsp < TS_PASS_MAX) { vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 3 + 2 * c->ntsp); c->ntsp++; }
    // 附件在通道里用的布局由 make_pass 定（缺省全程 GENERAL，开 nvk_opt_layout 时用附件专用布局），通道内不迁移：
    // 出通道后本段第一次用到它时按需迁移（采样 / 存储 / 传输 / 再当附件），段末统一转回 GENERAL（lay_rest_at_commit）。
    // render pass / framebuffer 被已录制的命令引用，等 cmd 真正完成后再销毁（finish_cmd）。
}

static int ensure_push_capacity(struct nvn_gpu_cmd_s *c, int st, uint32_t required) {
    if (required <= c->push_capacity[st]) return 1;
    if (required > g.caps.max_push_bytes) { set_sticky(c, NVN_GPU_E_ARG, "push 暂存超出设备上限"); return 0; }
    uint8_t *data = realloc(c->push_data[st], required);
    if (!data) { set_sticky(c, NVN_GPU_E_OOM, "push 暂存分配失败"); return 0; }
    memset(data + c->push_capacity[st], 0, required - c->push_capacity[st]);
    c->push_data[st] = data;
    c->push_capacity[st] = required;
    return 1;
}

void nvn_gpu_set_pipe(nvn_gpu_cmd ch, nvn_gpu_pipe ph) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_pipe_s *p = ph;
    if (!p) { set_sticky(c, NVN_GPU_E_ARG, "set_pipe 传了空管线"); return; }
    if (!c->in_pass && p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "图形管线在通道外 set_pipe"); return; }
    // Already allocated stage data survives pipe changes. Expand before the next push/dispatch.
    for (int st = 0; st <= NVN_GPU_SH_CS; st++) {
        uint64_t end = (uint64_t)p->stage_push_off[st] + p->stage_push_bytes[st];
        if (c->push_data[st] && end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
        if (c->push_data[st] && !ensure_push_capacity(c, st, (uint32_t)end)) return;
    }
    c->pipe = ph;
}

void nvn_gpu_set_dyn(nvn_gpu_cmd ch, const nvn_gpu_dyn *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "set_dyn 在通道外"); return; }
    if (!d) { set_sticky(c, NVN_GPU_E_ARG, "set_dyn 为空"); return; }
    c->dyn = *d;
    c->has_dyn = 1;
}

void nvn_gpu_set_vbuf(nvn_gpu_cmd ch, uint32_t stream, nvn_gpu_buf bh, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (stream >= 16) { set_sticky(c, NVN_GPU_E_ARG, "顶点流 %u 越界", stream); return; }
    c->vbuf[stream].buf = bh;
    c->vbuf[stream].off = off;
    if (bh) cmd_ref(c, bh, REF_BUF);
}

// 先按地址查活视图表，再保活，绝不把旧 u.b 的 buffer 指针当作 view 解引用。
static int cmd_ref_texel_view(struct nvn_gpu_cmd_s *c, nvn_gpu_view handle, uint32_t cls) {
    VkFormatFeatureFlags need = cls == NVN_GPU_RES_TBUF ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                                         : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    uint32_t cap = cls == NVN_GPU_RES_TBUF ? NVN_GPU_FMT_SAMPLE : NVN_GPU_FMT_STORAGE;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_view_s *v = g.views;
    while (v && v != handle) v = v->all_next;
    if (!v || v->released || !v->buffer_view || !(v->buffer_features & need) || !(v->buffer_caps & cap)) {
        pthread_mutex_unlock(&g_mu);
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素缓冲绑定不是支持该用途的有效缓冲视图");
        return 0;
    }
    v->refs++;
    pthread_mutex_unlock(&g_mu);
    if (c->nrefs == c->refcap) {
        uint32_t cap = c->refcap ? c->refcap * 2 : 16;
        struct vk_ref *p = realloc(c->refs, (size_t)cap * sizeof *p);
        if (!p) {
            pthread_mutex_lock(&g_mu); unref_locked(&g, v, REF_VIEW); pthread_mutex_unlock(&g_mu);
            set_sticky(c, NVN_GPU_E_OOM, "引用表分配失败");
            return 0;
        }
        c->refs = p; c->refcap = cap;
    }
    c->refs[c->nrefs++] = (struct vk_ref){v, REF_VIEW};
    return 1;
}

// 不比较 union 的未用字节 / padding；缓冲偏移、范围及数组元素都是绑定身份的一部分。
static int binding_equal(const nvn_gpu_binding *a, const nvn_gpu_binding *b) {
    if (a->ent != b->ent || a->elem != b->elem || a->cls != b->cls) return 0;
    switch (a->cls) {
    case NVN_GPU_RES_UBO: case NVN_GPU_RES_SSBO:
        return a->u.b.buf == b->u.b.buf && a->u.b.off == b->u.b.off && a->u.b.len == b->u.b.len;
    case NVN_GPU_RES_TEX: case NVN_GPU_RES_IMAGE: case NVN_GPU_RES_TBUF: case NVN_GPU_RES_IMAGE_TBUF: return a->u.view == b->u.view;
    case NVN_GPU_RES_SAMP: return a->u.samp == b->u.samp;
    case NVN_GPU_RES_TEX_SAMP: return a->u.ts.view == b->u.ts.view && a->u.ts.samp == b->u.ts.samp;
    default: return 0;
    }
}

void nvn_gpu_bind(nvn_gpu_cmd ch, nvn_gpu_stage st, const nvn_gpu_binding *b, uint32_t n) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass && (!c->pipe || c->pipe->pass)) { set_sticky(c, NVN_GPU_E_ORDER, "bind 在通道外（非计算管线）"); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "bind 前没有 set_pipe"); return; }
    if (!b && n) { set_sticky(c, NVN_GPU_E_ARG, "bind 表为空"); return; }
    if (n > 64) { set_sticky(c, NVN_GPU_E_ARG, "一个阶段一次绑定 %u 项，超过上限 64（原先多出来的被悄悄丢掉）", n); return; }
    if ((uint32_t)st > NVN_GPU_SH_CS) { set_sticky(c, NVN_GPU_E_ARG, "bind 阶段越界"); return; }
    if (c->desc_layout != c->pipe->layout) c->desc_dirty = 0xfu;
    else {
        uint32_t max = n > c->nbinds[st] ? n : c->nbinds[st];
        for (uint32_t i = 0; i < max; i++) {
            const nvn_gpu_binding *old = i < c->nbinds[st] ? &c->binds[st][i].b : NULL, *next = i < n ? &b[i] : NULL;
            if (old && next && binding_equal(old, next)) continue;
            if (old && old->ent < c->pipe->stage_nent[st]) c->desc_dirty |= 1u << c->pipe->stage_ents[st][old->ent].set;
            if (next && next->ent < c->pipe->stage_nent[st]) c->desc_dirty |= 1u << c->pipe->stage_ents[st][next->ent].set;
        }
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        if (c->pipe->stage_nent[st] && b[i].ent >= c->pipe->stage_nent[st]) {
            set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 的 ent %u 越界", i, b[i].ent);
            return;
        }
        const nvn_gpu_layout_ent *e = c->pipe->stage_nent[st] ? &c->pipe->stage_ents[st][b[i].ent] : NULL;
        if (e && e->cls != b[i].cls) { set_sticky(c, NVN_GPU_E_LAYOUT, "绑定项 %u 类别 %u != 布局 %u", i, b[i].cls, e->cls); return; }
        c->binds[st][count].b = b[i];
        if (b[i].cls == NVN_GPU_RES_TEX_SAMP) {
            if (!b[i].u.ts.view || !b[i].u.ts.samp) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp");
                return;
            }
            cmd_ref(c, b[i].u.ts.view, REF_VIEW);
            cmd_ref(c, b[i].u.ts.samp, REF_SAMP);
        } else if (b[i].cls == NVN_GPU_RES_TEX && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_SAMP && b[i].u.samp) cmd_ref(c, b[i].u.samp, REF_SAMP);
        else if (b[i].cls == NVN_GPU_RES_IMAGE && b[i].u.view) cmd_ref(c, b[i].u.view, REF_VIEW);
        else if (b[i].cls == NVN_GPU_RES_TBUF || b[i].cls == NVN_GPU_RES_IMAGE_TBUF) {
            if (!cmd_ref_texel_view(c, b[i].u.view, b[i].cls)) return;
        }
        else if ((b[i].cls == NVN_GPU_RES_UBO || b[i].cls == NVN_GPU_RES_SSBO) && b[i].u.b.buf) cmd_ref(c, b[i].u.b.buf, REF_BUF);
        count++;
    }
    c->nbinds[st] = count;
}

// 测试读数：各类状态命令实际录了几次（下标同 ES_*：管线、视口、裁剪、混合常量、模板、深度偏移、顶点缓冲、索引缓冲、push）
void nvn_gpu_vk_test_emit_counts(nvn_gpu_cmd ch, uint32_t out[9]) {
    struct nvn_gpu_cmd_s *c = ch;
    for (int i = 0; i < 9; i++) out[i] = c && i < ES_KINDS ? c->es_count[i] : 0;
}

// Test-only observation of the real allocation; never reads or writes push bytes.
uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd ch, nvn_gpu_stage st) {
    struct nvn_gpu_cmd_s *c = ch;
    return c && (uint32_t)st <= (uint32_t)NVN_GPU_SH_CS ? c->push_capacity[st] : 0;
}

void nvn_gpu_push(nvn_gpu_cmd ch, nvn_gpu_stage st, uint32_t off, const void *data, uint32_t bytes) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if ((uint32_t)st > (uint32_t)NVN_GPU_SH_CS) { set_sticky(c, NVN_GPU_E_ARG, "push 阶段 %d 非法", (int)st); return; }
    if (!c->pipe) { set_sticky(c, NVN_GPU_E_ORDER, "push 前没有 set_pipe"); return; }
    if (!c->in_pass && c->pipe->pass) { set_sticky(c, NVN_GPU_E_ORDER, "push 在通道外（非计算管线）"); return; }
    if (!data && bytes) { set_sticky(c, NVN_GPU_E_ARG, "push data 为空"); return; }
    uint32_t po = c->pipe->stage_push_off[st], pb = c->pipe->stage_push_bytes[st];
    if (!pb) { set_sticky(c, NVN_GPU_E_ARG, "阶段 %d 未声明 push 范围", (int)st); return; }
    if (off < po || (uint64_t)(off - po) + bytes > pb) {
        set_sticky(c, NVN_GPU_E_ARG, "push [%u,+%u) 越出阶段 %d 声明范围 [%u,+%u)", off, bytes, (int)st, po, pb);
        return;
    }
    if (!bytes) return;
    uint64_t end = (uint64_t)po + pb;
    if (end > UINT32_MAX) { set_sticky(c, NVN_GPU_E_ARG, "push 范围溢出"); return; }
    if (!ensure_push_capacity(c, st, (uint32_t)end)) return;
    if (!c->push_have[st] || memcmp(c->push_data[st] + off, data, bytes)) { memcpy(c->push_data[st] + off, data, bytes); c->push_gen[st]++; }
    c->push_have[st] = 1;
}

// ---------------------------------------------------------------- 描述符池 / 绑定写入 / texel 视图

// 每条 cmd 一个池：64 set（缺扩容是已知静默错 C66 的复现对象，本段不修）。
// 一段命令的描述符池：一池 256 组，分满了换新池接着分（旧池留到段完成再销毁），一段里画多少笔都不设上限。
// 池的上限按规范可以被驱动放宽也可以严格执行，所以「分满」只认分配调用返回的池耗尽 / 碎片两种结果，不自己数。
enum { DPOOL_SETS = 256, DPOOL_PER_TYPE = 1024 };
static int new_dpool(struct nvn_gpu_cmd_s *c) {
    VkDescriptorPoolSize ps[8] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_SAMPLER, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DPOOL_PER_TYPE},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, DPOOL_PER_TYPE}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, DPOOL_PER_TYPE}};
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = DPOOL_SETS;
    dpi.poolSizeCount = 8;
    dpi.pPoolSizes = ps;
    if (seg_reuse_on() && (c->dpool = seg_take_desc()) != VK_NULL_HANDLE) return 1;
    OC(OC_DPOOL);
    if (vkCreateDescriptorPool(g.d, &dpi, NULL, &c->dpool) != VK_SUCCESS) {
        c->dpool = VK_NULL_HANDLE;
        set_sticky(c, NVN_GPU_E_OOM, "描述符池创建失败");
        return 0;
    }
    return 1;
}
// 给一笔绘制 / 派发分配它的 n 组描述符。0 = 失败（已记粘滞错误）
static int alloc_sets(struct nvn_gpu_cmd_s *c, const VkDescriptorSetLayout *dsl, uint32_t n, VkDescriptorSet *sets) {
    for (int fresh = 0; fresh < 2; fresh++) {
        if (!c->dpool && !new_dpool(c)) return 0;
        VkDescriptorSetAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = c->dpool;
        ai.descriptorSetCount = n;
        ai.pSetLayouts = dsl;
        OC(OC_DALLOC);
        VkResult r = vkAllocateDescriptorSets(g.d, &ai, sets);
        if (r == VK_SUCCESS) return 1;
        if (fresh || (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)) {
            set_sticky(c, NVN_GPU_E_OOM, "vkAllocateDescriptorSets 失败（%d%s）", (int)r, fresh ? "，新池上也分不出" : "");
            return 0;
        }
        // 当前池分满：挪进「已满」表，下一轮在新池上重分这一笔
        if (c->ndpools_full == c->dpools_full_cap) {
            uint32_t cap = c->dpools_full_cap ? c->dpools_full_cap * 2 : 8;
            VkDescriptorPool *np = realloc(c->dpools_full, cap * sizeof *np);
            if (!np) { set_sticky(c, NVN_GPU_E_OOM, "描述符池表分配失败"); return 0; }
            c->dpools_full = np;
            c->dpools_full_cap = cap;
        }
        c->dpools_full[c->ndpools_full++] = c->dpool;
        c->dpool = VK_NULL_HANDLE;
    }
    return 0;
}

// 采样绑定在描述符里声明的布局必须等于这一笔执行时图像的实际布局：本段跟踪的 GENERAL / *READ_ONLY 二选一。
static VkImageLayout bind_image_layout(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v) {
    return c->opt && v && v->tex ? lay_cur(c, v->tex) : VK_IMAGE_LAYOUT_GENERAL;
}
// 把某阶段本笔绑定写进 wr/ii/bi；返回写入条数，出错置粘滞返回 -1。
// 缺成员的新类别（IMAGE / TBUF / IMAGE_TBUF）报 E_LAYOUT；老类别保持 NULL 句柄空资源写法不变。
// 只写 dirty 里的组（没变的组沿用上一笔的描述符集对象，不重写）。纹素缓冲的格式核对取决于管线，
// 组没变、管线换了也要核，所以这两类不在脏组里也走一遍核对，只是不写。
static int write_binds(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int st, uint32_t dirty,
                       const VkDescriptorSet *sets, VkWriteDescriptorSet *wr,
                       VkDescriptorImageInfo *ii, VkDescriptorBufferInfo *bi) {
    int nw = 0;
    for (uint32_t i = 0; i < c->nbinds[st]; i++) {
        const nvn_gpu_binding *b = &c->binds[st][i].b;
        if (!p->stage_nent[st]) { set_sticky(c, NVN_GPU_E_LAYOUT, "阶段 %d 没有布局项却给了绑定", st); return -1; }
        const nvn_gpu_layout_ent *e = &p->stage_ents[st][b->ent];
        const int live = (int)(dirty >> e->set & 1u);
        if (!live && e->cls != NVN_GPU_RES_TBUF && e->cls != NVN_GPU_RES_IMAGE_TBUF) continue;
        VkWriteDescriptorSet *w = &wr[nw];
        memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = sets[e->set];
        w->dstBinding = e->binding;
        w->dstArrayElement = b->elem;
        w->descriptorCount = 1;
        w->descriptorType = vk_desc_type(e->cls);
        VkDescriptorImageInfo *im = &ii[nw];
        VkDescriptorBufferInfo *bf = &bi[nw];
        switch (e->cls) {
        case NVN_GPU_RES_TEX:
            memset(im, 0, sizeof *im);
            im->imageView = b->u.view ? b->u.view->view : VK_NULL_HANDLE;
            im->imageLayout = bind_image_layout(c, b->u.view);
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_SAMP:
            memset(im, 0, sizeof *im);
            im->sampler = b->u.samp ? b->u.samp->sampler : VK_NULL_HANDLE;
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_TEX_SAMP:
            if (!b->u.ts.view || !b->u.ts.samp) { set_sticky(c, NVN_GPU_E_LAYOUT, "TEX_SAMP 绑定缺 view 或 samp"); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = b->u.ts.view->view;
            { nvn_fmt_info vfi; int vint = nvn_fmt_info_of(b->u.ts.view->nvn_format, &vfi) && (vfi.kind == NVN_FMT_UINT || vfi.kind == NVN_FMT_SINT);
              im->sampler = vint ? b->u.ts.samp->int_sampler : b->u.ts.samp->sampler; }
            im->imageLayout = bind_image_layout(c, b->u.ts.view);
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_IMAGE:
            if (!b->u.view) { set_sticky(c, NVN_GPU_E_LAYOUT, "IMAGE 绑定缺 view"); return -1; }
            memset(im, 0, sizeof *im);
            im->imageView = view_image_form(b->u.view);   // 带通道映射的视图不能当存储图像用：另建恒等形态
            im->imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // 存储图像一律 GENERAL（Vulkan 要求），迁移见 prepare_bind_layouts
            w->pImageInfo = im;
            break;
        case NVN_GPU_RES_UBO:
        case NVN_GPU_RES_SSBO:
            memset(bf, 0, sizeof *bf);
            bf->buffer = b->u.b.buf ? b->u.b.buf->buffer : VK_NULL_HANDLE;
            bf->offset = b->u.b.off;
            bf->range = b->u.b.len;
            if (e->cls == NVN_GPU_RES_UBO && b->u.b.len > g.caps.max_ubo_range) {   // UBO 没有 WHOLE_SIZE 例外：超过范围上限只能准确拒绝
                set_sticky(c, NVN_GPU_E_ARG, "常量缓冲范围 %llu 超过 maxUniformBufferRange %llu", (unsigned long long)b->u.b.len, (unsigned long long)g.caps.max_ubo_range);
                return -1; }
            if (e->cls == NVN_GPU_RES_SSBO && b->u.b.len > g.caps.max_ssbo_range) {
                // Vulkan 的存储缓冲范围上限管的是「范围」而不是缓冲大小：超过上限的绑定用 VK_WHOLE_SIZE
                // （规范只约束非 WHOLE_SIZE 的 range），有效范围变成 offset 到缓冲尾。这是平台分叉，报一次红行。
                static int said; if (!said++) {
                    char m[160]; snprintf(m, sizeof m, "存储缓冲范围 %llu 超过 maxStorageBufferRange %llu：按 VK_WHOLE_SIZE 绑定（Vulkan 无更大范围）",
                                          (unsigned long long)b->u.b.len, (unsigned long long)g.caps.max_ssbo_range);
                    nvn_plat_log(NVN_PLAT_LOG_RED, "gpu", m); }
                bf->range = VK_WHOLE_SIZE; }
            w->pBufferInfo = bf;
            break;
        case NVN_GPU_RES_TBUF:
        case NVN_GPU_RES_IMAGE_TBUF: {
            struct nvn_gpu_view_s *v = b->u.view; // 已在 bind 时核验并保活
            VkFormat f = p->stage_tfmt[st] ? p->stage_tfmt[st][b->ent] : VK_FORMAT_UNDEFINED;
            if (f != VK_FORMAT_UNDEFINED && f != v->buffer_format) {
                set_sticky(c, NVN_GPU_E_LAYOUT, "texel 绑定视图格式与着色器声明不符");
                return -1;
            }
            if (e->cls == NVN_GPU_RES_IMAGE_TBUF && f == VK_FORMAT_UNDEFINED &&
                !texel_unknown_storage_supported(v->buffer_format)) {
                set_sticky(c, NVN_GPU_E_UNSUPPORTED, "纹素视图格式不支持无格式存储访问");
                return -1;
            }
            w->pTexelBufferView = &v->buffer_view;
            break;
        }
        default:
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "绑定类别 %u：本段未实现", e->cls);
            return -1;
        }
        if (live) nw++;
    }
    return nw;
}

// 脏组分配新对象，不能更新前一笔已经引用的组；跨阶段共用同一组时写全组。
static int bind_descriptor_groups(struct nvn_gpu_cmd_s *c, const struct nvn_gpu_pipe_s *p, int compute) {
    const uint8_t point = compute ? 2 : 1;
    if (c->desc_layout != p->layout || c->desc_point != point) {
        c->desc_layout = p->layout; c->desc_point = point; c->desc_dirty = 0xfu;
        memset(c->desc_sets, 0, sizeof c->desc_sets);
    }
    // A descriptor-free pipeline ignores bindings left by the previous pipeline.
    if (!p->nset) return 1;
    uint32_t dirty = c->desc_dirty & ((1u << p->nset) - 1u);
    // 哪一组都没变、布局和绑定点也没换：命令缓冲里绑着的就是这几组，不用重写也不用重绑。
    // 纹素缓冲的格式核对随管线变（见 write_binds），有这类绑定的管线不走这条近路。
    struct nvn_gpu_pipe_s *pm = (struct nvn_gpu_pipe_s *)(uintptr_t)p;
    if (!pm->texel_binds) {
        int8_t has = -1;
        for (int st = 0; st <= NVN_GPU_SH_CS; st++) for (uint32_t i = 0; i < p->stage_nent[st]; i++)
            if (p->stage_ents[st][i].cls == NVN_GPU_RES_TBUF || p->stage_ents[st][i].cls == NVN_GPU_RES_IMAGE_TBUF) has = 1;
        pm->texel_binds = has;
    }
    if (!dirty && p->texel_binds < 0) return 1;
    const int tm = g_draw_tm; const uint64_t t0 = tm ? now_ns() : 0;   // 诊断：这四步各用多久（nvk_gputime 开着才计）
    // 描述符组备忘（调试开关 nvk_desc_memo，缺省关；照 d2r-mobile 安卓笔记 perf-round2 发现 4）：
    // 同一段里内容完全相同的组（同一布局、每条描述符的绑定号 / 类型 / 资源都相同）直接用上次写好的那一组，不再分配、不再写。
    // 一款 3D 游戏每帧重写 2800 组、1.13 万条描述符（分配 2.3 毫秒 + 写入 2.05 毫秒）。组归这一段的描述符池，段做完才销毁；
    // 内容相同就是资源相同，资源由首次写入那笔的引用保活到段完成。
    static int memo_cur = -1; static uint32_t memo_k;
    if (memo_cur < 0 || !(++memo_k & 255)) memo_cur = nvn_plat_flag("nvk_desc_memo", 0) != 0;
    const int memo = memo_cur && dirty;
    VkDescriptorSet mark[4];   // 备忘开着时先不分配：写入项里先放记号，认出每条属于哪一组
    if (memo) { for (uint32_t set = 0; set < 4; set++) mark[set] = (VkDescriptorSet)(uintptr_t)(0x5a0 + set); }
    else for (uint32_t set = 0; set < p->nset; set++)
        if (dirty & (1u << set)) if (!alloc_sets(c, p->dsl + set, 1, c->desc_sets + set)) return 0;
    const uint64_t t1 = tm ? now_ns() : 0;
    VkWriteDescriptorSet wr[6 * 64]; VkDescriptorImageInfo ii[6 * 64]; VkDescriptorBufferInfo bi[6 * 64];
    uint32_t nw = 0;
    for (int st = compute ? NVN_GPU_SH_CS : 0; st < (compute ? NVN_GPU_SH_CS + 1 : 5); st++) {
        int n = write_binds(c, p, st, dirty, memo ? mark : c->desc_sets, wr + nw, ii + nw, bi + nw);
        if (n < 0) return 0;
        nw += (uint32_t)n;
    }
    if (memo) {
        uint64_t h1[4], h2[4]; uint32_t hit = 0;
        for (uint32_t set = 0; set < 4; set++) { h1[set] = 1469598103934665603ull ^ (uintptr_t)(set < p->nset ? p->dsl[set] : 0); h2[set] = 0x9E3779B97F4A7C15ull + set; }
        for (uint32_t i = 0; i < nw; i++) {
            const uint32_t set = (uint32_t)((uintptr_t)wr[i].dstSet - 0x5a0); if (set >= 4) continue;
            uint64_t v[5] = {(uint64_t)wr[i].dstBinding << 32 | wr[i].dstArrayElement, (uint64_t)wr[i].descriptorType, 0, 0, 0};
            if (wr[i].pImageInfo) { v[2] = (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView; v[3] = (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler; v[4] = (uint64_t)wr[i].pImageInfo->imageLayout; }
            else if (wr[i].pBufferInfo) { v[2] = (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer; v[3] = wr[i].pBufferInfo->offset; v[4] = wr[i].pBufferInfo->range; }
            else if (wr[i].pTexelBufferView) v[2] = (uint64_t)(uintptr_t)*wr[i].pTexelBufferView;
            for (int k = 0; k < 5; k++) { h1[set] = (h1[set] ^ v[k]) * 1099511628211ull; h2[set] = (h2[set] + v[k]) * 0xff51afd7ed558ccdull; h2[set] ^= h2[set] >> 29; }
        }
        if (!c->memo) c->memo = calloc(DESC_MEMO_N, sizeof *c->memo);
        for (uint32_t set = 0; set < p->nset; set++) {
            if (!(dirty & (1u << set))) continue;
            struct desc_memo *slot = NULL; int found = 0;
            if (c->memo) for (uint32_t q = 0; q < 16; q++) {
                struct desc_memo *m = &c->memo[(h1[set] + q) % DESC_MEMO_N];
                if (!m->set) { slot = m; break; }
                if (m->h1 == h1[set] && m->h2 == h2[set] && m->dsl == p->dsl[set]) { c->desc_sets[set] = m->set; found = 1; break; }
            }
            if (found) { hit |= 1u << set; continue; }
            if (!alloc_sets(c, p->dsl + set, 1, c->desc_sets + set)) return 0;
            if (slot && c->memo_n < DESC_MEMO_N * 3 / 4) { slot->h1 = h1[set]; slot->h2 = h2[set]; slot->dsl = p->dsl[set]; slot->set = c->desc_sets[set]; c->memo_n++; }
        }
        uint32_t keep = 0;   // 命中的组不用写：把它们的写入项去掉，其余的换上真的组
        for (uint32_t i = 0; i < nw; i++) {
            const uint32_t set = (uint32_t)((uintptr_t)wr[i].dstSet - 0x5a0);
            if (set >= 4 || (hit >> set & 1)) continue;
            wr[keep] = wr[i]; wr[keep].dstSet = c->desc_sets[set]; keep++;
        }
        nw = keep;
        if (tm) __atomic_fetch_add(&g_desc_hit, (uint64_t)__builtin_popcount(hit), __ATOMIC_RELAXED);
    }
    // 只重写变过的条目（切换 nvk_desc_skip 时整份写）。影子按「组对象 + binding 槽」记：换了组对象就整组作废。
    {
        static int skip_cur = -1; static uint32_t skip_k;
        if (skip_cur < 0 || !(++skip_k & 255)) skip_cur = nvn_plat_flag("nvk_desc_skip", 1) != 0;
        if (skip_cur && !memo) {
            for (uint32_t set = 0; set < 4; set++) {
                if (!(c->desc_sets[set] && (c->dsh_ok[set] == 0 || c->dsh_set[set] != c->desc_sets[set]))) continue;
                memset(c->dsh[set], 0, sizeof c->dsh[set]); c->dsh_set[set] = c->desc_sets[set]; c->dsh_ok[set] = 1;
            }
            uint32_t keep = 0;
            for (uint32_t i = 0; i < nw; i++) {
                uint32_t set = 4;
                for (uint32_t s = 0; s < 4; s++) if (c->desc_sets[s] && wr[i].dstSet == c->desc_sets[s]) { set = s; break; }
                if (set >= 4 || wr[i].dstBinding >= 32) { wr[keep++] = wr[i]; continue; }
                struct desc_shadow *sh = &c->dsh[set][wr[i].dstBinding];
                struct desc_shadow nx; memset(&nx, 0, sizeof nx);
                nx.type = wr[i].descriptorType; nx.binding = (uint16_t)wr[i].dstBinding;
                if (wr[i].pImageInfo) { nx.a = (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView; nx.b = (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler; nx.c = (uint64_t)wr[i].pImageInfo->imageLayout; }
                else if (wr[i].pBufferInfo) { nx.a = (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer; nx.b = wr[i].pBufferInfo->offset; nx.c = wr[i].pBufferInfo->range; }
                else if (wr[i].pTexelBufferView) nx.a = (uint64_t)(uintptr_t)*wr[i].pTexelBufferView;
                if (sh->valid && sh->a == nx.a && sh->b == nx.b && sh->c == nx.c && sh->type == nx.type) continue;
                nx.valid = 1; *sh = nx; wr[keep++] = wr[i];
            }
            nw = keep;
        }
    }
    const uint64_t t2 = tm ? now_ns() : 0;
    if (nw) { OC(OC_DWRITE); vkUpdateDescriptorSets(g.d, nw, wr, 0, NULL); }
    const uint64_t t3 = tm ? now_ns() : 0;
    c->desc_dirty = 0;
    if (dirty && p->nset) vkCmdBindDescriptorSets(c->cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  p->layout, 0, p->nset, c->desc_sets, 0, NULL);
    if (tm) { const uint64_t t4 = now_ns();
        __atomic_fetch_add(&g_desc_ns[0], t1 - t0, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_ns[1], t2 - t1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_desc_ns[2], t3 - t2, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_ns[3], t4 - t3, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_desc_nw, nw, __ATOMIC_RELAXED); __atomic_fetch_add(&g_desc_sets, (uint64_t)__builtin_popcount(dirty), __ATOMIC_RELAXED); }
    return 1;
}

// 绘制 / 派发前：把这一笔要采样 / 当存储图像用的图按本段的布局状态惰性迁移过去。
// 开通道时 pass_flush_pending 已经把「附件布局」都转成只读采样布局，所以通道里采样不需要迁移；
// 这里只剩通道里新出现的两种要迁：本通道的附件又要被采样（反馈环）→ 这张图这一段退回 GENERAL；
// 存储图像要 GENERAL 而它现在是只读布局。通道内不许迁布局，所以都得在通道外做：重开通道（见 pass_reopen）。
static int prepare_bind_layouts(struct nvn_gpu_cmd_s *c, int compute) {
    if (!c->opt) return 1;
    const int st0 = compute ? NVN_GPU_SH_CS : 0, st1 = compute ? NVN_GPU_SH_CS + 1 : 5;
    struct reopen_fix fx[32]; uint32_t nfx = 0;
    for (int st = st0; st < st1; st++)
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            struct nvn_gpu_tex_s *t = bind_tex_of(b);
            if (!t) continue;
            const VkImageLayout cur = lay_cur(c, t);
            if (pass_attached(c, t)) {
                if (cur == VK_IMAGE_LAYOUT_GENERAL) continue;   // 这一段本来就 GENERAL：采样 / 存储都能用
                t->fb_sample = 1;                               // 反馈环：这张图这一段退回 GENERAL（以后当附件一律 GENERAL）
                if (nfx < 32) { fx[nfx].t = t; fx[nfx].want = VK_IMAGE_LAYOUT_GENERAL; nfx++; }
                continue;
            }
            const int storage = b->cls == NVN_GPU_RES_IMAGE;
            const VkImageLayout want = storage ? VK_IMAGE_LAYOUT_GENERAL : sample_layout_for(t);
            if (cur == want || cur == VK_IMAGE_LAYOUT_GENERAL) continue;   // GENERAL 采样 / 存储都合法，不动它
            if (nfx < 32) { fx[nfx].t = t; fx[nfx].want = want; nfx++; }
        }
    if (!nfx) return rc_is(c->sticky, NVN_GPU_OK);
    if (!c->in_pass) {   // 通道外（计算派发）：直接迁
        for (uint32_t i = 0; i < nfx; i++) image_barrier(c, fx[i].t, fx[i].want);
        return rc_is(c->sticky, NVN_GPU_OK);
    }
    if (!pass_reopen(c, fx, nfx)) {
        if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_ORDER, "通道里要改布局（反馈环 / 存储图像），通道又拆不开");
        return 0;
    }
    return rc_is(c->sticky, NVN_GPU_OK);
}

void nvn_gpu_draw(nvn_gpu_cmd ch, const nvn_gpu_draw_args *a) {
    draw_tm_init();   // 诊断：这笔绘制各段用时（nvk_gputime 开着才计）
    const int dtm = g_draw_tm; const uint64_t dt0 = dtm ? now_ns() : 0;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass || !c->pipe || !a) { set_sticky(c, NVN_GPU_E_ORDER, "draw 缺通道 / 管线 / 参数"); return; }
    if (c->tsq && c->ntsp < TS_PASS_MAX) c->tsp[c->ntsp].draws++;
    if (a->cnt) {
        if (!(g.caps.caps & NVN_GPU_CAP_INDIRECT_COUNT)) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "draw_indirect_count：设备未启用 KHR count / multiDrawIndirect / firstInstance"); return;
        }
        uint32_t size = a->ibuf ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
        uint32_t stride = a->stride ? a->stride : size;
        uint64_t span = a->max_draws ? (uint64_t)(a->max_draws - 1) * stride + size : 0;
        if (!a->ind || (a->cnt_off & 3) || a->cnt_off > a->cnt->bytes || a->cnt->bytes - a->cnt_off < 4 ||
            (a->ind_off & 3) || (stride & 3) || stride < size ||
            a->max_draws > g.dev->properties.limits.maxDrawIndirectCount ||
            a->ind_off >= a->ind->bytes || span > a->ind->bytes - a->ind_off ||
            (a->ibuf && (a->itype > 2 || a->ioff >= a->ibuf->bytes ||
                         (a->ioff & ((1u << a->itype) - 1))))) {
            set_sticky(c, NVN_GPU_E_ARG, "draw_indirect_count：偏移、步长、最大跨度或设备上限非法"); return;
        }
        // GPU-produced count/commands are authoritative. The producer must obey Vulkan's device limit.
        cmd_ref(c, a->cnt, REF_BUF);
        if (!rc_is(c->sticky, NVN_GPU_OK)) return;
    }
    if (a->prim != 0x0 && a->prim != 0x1 && a->prim != 0x3 && a->prim != 0x4 && a->prim != 0x5 && a->prim != 0x6) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "图元 %u 未实现（线环 / quads 等）", a->prim); return;
    }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (a->prim != p->key.prim) {
        set_sticky(c, NVN_GPU_E_ARG, "draw prim %u 与管线拓扑 %u 不一致", a->prim, p->key.prim);
        return;
    }
    if (!prepare_bind_layouts(c, 0)) return;   // 采样 / 存储图的布局先到位（可能拆通道），再写描述符
    if (!bind_descriptor_groups(c, p, 0)) return;
    const uint64_t dt1 = dtm ? now_ns() : 0;
    // 以下各项只在和上次真正录进去的不同时才录（#708；影子见 nvn_gpu_cmd_s.es）
    for (int st = 0; st < 5; st++)
        if (c->push_have[st] && p->stage_push_bytes[st]) {
            if (c->es.push_ok[st] && c->es.push_layout[st] == p->layout && c->es.push_off[st] == p->stage_push_off[st] &&
                c->es.push_bytes[st] == p->stage_push_bytes[st] && c->es.push_gen[st] == c->push_gen[st]) continue;
            vkCmdPushConstants(c->cb, p->layout, vk_shader_stage((nvn_gpu_stage)st),
                               p->stage_push_off[st], p->stage_push_bytes[st], c->push_data[st] + p->stage_push_off[st]);
            c->es.push_ok[st] = 1; c->es.push_layout[st] = p->layout; c->es.push_off[st] = p->stage_push_off[st];
            c->es.push_bytes[st] = p->stage_push_bytes[st]; c->es.push_gen[st] = c->push_gen[st]; c->es_count[ES_PUSH]++;
        }
    if (c->has_dyn && (c->dyn.origin_ll != 0) != (p->key.origin_ll != 0)) { set_sticky(c, NVN_GPU_E_ARG, "绘制时的窗口原点（左下 = %d）与管线描述里的不一致", (int)c->dyn.origin_ll); return; }
    // 换游戏管线不动动态状态：所有游戏管线声明的动态状态是同一组（见建管线处的 dyn[7]），按规范切换时保留；
    // 只声明了一部分的管线（矩形清除的）用过之后整份影子作废（nvn_gpu_clear_rect）。
    if (!c->es.gpipe || c->es.gpipe != p->pipe) { vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe); c->es.gpipe = p->pipe; c->es_count[ES_PIPE]++; }
    VkViewport vp;
    if (c->has_dyn && c->dyn.nvp) {
        vp = (VkViewport){c->dyn.vp[0][0], c->dyn.vp[0][1], c->dyn.vp[0][2], c->dyn.vp[0][3], c->dyn.vp[0][4], c->dyn.vp[0][5]};
    } else {
        vp = (VkViewport){0, 0, (float)c->pass_w, (float)c->pass_h, 0.0f, 1.0f};
    }
    // NVN 窗口原点在左上时，裁剪空间 +y 落在图的上方（行号小的一边）；Vulkan 的 +y 朝下（行号大的一边）：用负高度的视口翻过来。
    // 原点在左下（NVN 默认）时两边的行号方向一致，不翻；那种图是自下而上存的，由呈现时翻（present_desc.flip_y）。
    if (c->has_dyn && !c->dyn.origin_ll) { vp.y += vp.height; vp.height = -vp.height; }
    VkRect2D sc = {{0, 0}, {c->pass_w, c->pass_h}};
    if (c->has_dyn && c->dyn.has_sc)
        sc = (VkRect2D){{c->dyn.sc[0], c->dyn.sc[1]}, {(uint32_t)c->dyn.sc[2], (uint32_t)c->dyn.sc[3]}};
    // 浮点按位比（memcmp）：-0 与 +0、NaN 各按自己的位，宁可多发不漏发
    if (!c->es.vp_ok || memcmp(&c->es.vp, &vp, sizeof vp)) { vkCmdSetViewport(c->cb, 0, 1, &vp); c->es.vp = vp; c->es.vp_ok = 1; c->es_count[ES_VP]++; }
    if (!c->es.sc_ok || memcmp(&c->es.sc, &sc, sizeof sc)) { vkCmdSetScissor(c->cb, 0, 1, &sc); c->es.sc = sc; c->es.sc_ok = 1; c->es_count[ES_SC]++; }
    const float bc[4] = {c->dyn.blend_color[0], c->dyn.blend_color[1], c->dyn.blend_color[2], c->dyn.blend_color[3]};
    if (!c->es.bc_ok || memcmp(c->es.bc, bc, sizeof bc)) { vkCmdSetBlendConstants(c->cb, bc); memcpy(c->es.bc, bc, sizeof bc); c->es.bc_ok = 1; c->es_count[ES_BC]++; }
    // 模板的三个动态量：下标 0 = 正面、1 = 背面（NVN 的面位）。没发过动态状态时用 NVN 缺省（掩码全开、参考值 0）
    {
        uint32_t stv[2][3];
        for (int f = 0; f < 2; f++) {
            stv[f][0] = c->has_dyn ? c->dyn.st_vmask[f] & 0xffu : 0xffu;
            stv[f][1] = c->has_dyn ? c->dyn.st_wmask[f] & 0xffu : 0xffu;
            stv[f][2] = c->has_dyn ? c->dyn.st_ref[f] & 0xffu : 0u;
        }
        if (!c->es.st_ok || memcmp(c->es.st, stv, sizeof stv)) {
            for (int f = 0; f < 2; f++) {
                VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
                vkCmdSetStencilCompareMask(c->cb, face, stv[f][0]);
                vkCmdSetStencilWriteMask(c->cb, face, stv[f][1]);
                vkCmdSetStencilReference(c->cb, face, stv[f][2]);
            }
            memcpy(c->es.st, stv, sizeof stv); c->es.st_ok = 1; c->es_count[ES_ST]++;
        }
    }
    // 深度偏移（NVN SetPolygonOffsetClamp：factor 乘斜率、units 乘最小可分辨深度差、clamp 限幅）。管线没开偏移时这三个量不起作用，照发 0。
    {
        const p_poly_offset *po = &c->dyn.po;
        int on = c->has_dyn && (p->key.poly.offset_en & (1u << p->key.poly.mode));
        if (on && po->clamp != 0.0f && !g.dev->core_features.depthBiasClamp) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "深度偏移带限幅（clamp=%g），设备没有 depthBiasClamp", (double)po->clamp); return; }
        const float db[3] = {on ? po->units : 0.0f, on ? po->clamp : 0.0f, on ? po->factor : 0.0f};
        if (!c->es.db_ok || memcmp(c->es.db, db, sizeof db)) { vkCmdSetDepthBias(c->cb, db[0], db[1], db[2]); memcpy(c->es.db, db, sizeof db); c->es.db_ok = 1; c->es_count[ES_DB]++; }
    }
    const uint64_t dt2 = dtm ? now_ns() : 0;
    for (uint32_t s = 0; s < 16; s++) {
        if (!c->vbuf[s].buf) continue;
        VkBuffer vb = c->vbuf[s].buf->buffer;
        VkDeviceSize off = c->vbuf[s].off;
        if ((c->es.vb_ok >> s & 1) && c->es.vb[s] == vb && c->es.vboff[s] == off) continue;
        vkCmdBindVertexBuffers(c->cb, s, 1, &vb, &off);
        c->es.vb[s] = vb; c->es.vboff[s] = off; c->es.vb_ok |= (uint16_t)(1u << s); c->es_count[ES_VB]++;
    }
    uint32_t inst = a->instances ? a->instances : 1;
    uint32_t draws = a->max_draws ? a->max_draws : 1;
    if (a->ibuf) {
        if (a->itype > 2) { set_sticky(c, NVN_GPU_E_ARG, "索引类型 %u 非法", a->itype); return; }
        VkIndexType it = a->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : a->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        VkBuffer ib = a->ibuf->buffer;
        VkDeviceSize ioff = a->ioff;
        uint32_t first = a->first;
        // getenv 只在 u8 索引这一种（极少数）绘制上读：u16 / u32 的每笔绘制不再付这份开销（#950 的本意），
        //   又不把测试开关冻成首次读到的值（缓存成静态量后 C42 中途设的强制展开不生效）
        const int force_expand = a->itype == 0 && getenv("S2A_VK_FORCE_INDEX_U8_EXPAND") != NULL;
        if (a->itype == 0 && (!g.dev->index_type_uint8_enabled || force_expand)) {
            if (a->ind) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 间接索引无 CPU 展开回退"); return; }
            if (!a->ibuf->mapped || !a->count) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "u8 索引展开需要可映射索引缓冲"); return; }
            if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff)) {
                set_sticky(c, NVN_GPU_E_ARG, "u8 索引展开范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                           (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                return;
            }
            const uint8_t *src = a->ibuf->mapped + ioff + first;
            uint64_t need = 2ull * a->count;
            nvn_gpu_slice sl;
            nvn_gpu_err ae;
            if (!rc_is(nvn_gpu_arena_alloc(ch, need, 4, &sl, &ae), NVN_GPU_OK)) { set_sticky(c, ae.rc.code, "u8 索引展开暂存：%s", ae.why); return; }
            for (uint32_t i = 0; i < a->count; i++) ((uint16_t *)sl.cpu)[i] = src[i];
            ib = sl.buf->buffer;
            ioff = sl.off;
            it = VK_INDEX_TYPE_UINT16;
            first = 0;
        }
        {
            uint64_t isz = a->itype == 0 ? 1 : a->itype == 1 ? 2 : 4;
            // Native count reads firstIndex/indexCount from GPU commands, not stale direct-draw fields.
            if (!a->cnt && (a->itype != 0 || (g.dev->index_type_uint8_enabled && !force_expand))) {
                if (ioff > a->ibuf->bytes || (uint64_t)first + a->count > (a->ibuf->bytes - ioff) / isz) {
                    set_sticky(c, NVN_GPU_E_ARG, "索引范围越界（ioff=%llu first=%u count=%u bytes=%llu）",
                               (unsigned long long)ioff, first, a->count, (unsigned long long)a->ibuf->bytes);
                    return;
                }
            }
        }
        cmd_ref(c, a->ibuf, REF_BUF);
        if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
        if (!c->es.ib_ok || c->es.ib != ib || c->es.ioff != ioff || c->es.it != it) {
            vkCmdBindIndexBuffer(c->cb, ib, ioff, it);
            c->es.ib = ib; c->es.ioff = ioff; c->es.it = it; c->es.ib_ok = 1; c->es_count[ES_IB]++;
        }
        if (a->ind) {
            cmd_ref(c, a->ind, REF_BUF);
            if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
            uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndexedIndirectCommand);
            if (a->cnt) {
                vkCmdDrawIndexedIndirectCount(c->cb, a->ind->buffer, a->ind_off, a->cnt->buffer, a->cnt_off, a->max_draws, stride);
            } else if (draws > 1) {
                for (uint32_t i = 0; i < draws; i++)
                    vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride);
            } else {
                vkCmdDrawIndexedIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride);
            }
        } else {
            vkCmdDrawIndexed(c->cb, a->count, inst, first, a->base_vertex, a->base_instance);
        }
    } else if (a->ind) {
        cmd_ref(c, a->ind, REF_BUF);
        if (a->cnt && !rc_is(c->sticky, NVN_GPU_OK)) return;
        uint32_t stride = a->stride ? a->stride : (uint32_t)sizeof(VkDrawIndirectCommand);
        if (a->cnt) {
            vkCmdDrawIndirectCount(c->cb, a->ind->buffer, a->ind_off, a->cnt->buffer, a->cnt_off, a->max_draws, stride);
        } else if (draws > 1) {
            for (uint32_t i = 0; i < draws; i++)
                vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off + (uint64_t)i * stride, 1, stride);
        } else {
            vkCmdDrawIndirect(c->cb, a->ind->buffer, a->ind_off, 1, stride);
        }
    } else {
        vkCmdDraw(c->cb, a->count, inst, a->first, a->base_instance);
    }
    if (dtm) { const uint64_t dt3 = now_ns(); __atomic_fetch_add(&g_draw_ns[0], dt1 - dt0, __ATOMIC_RELAXED); __atomic_fetch_add(&g_draw_ns[1], dt2 - dt1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_draw_ns[2], dt3 - dt2, __ATOMIC_RELAXED);
        // 每 5 秒一次：把后端逐笔的三段（描述符 / 状态 / 顶点索引与绘制调用）摊到每笔上打印，
        // 找到后端这笔 5 微秒里到底是谁（真机读数见坑记录续 27）。
        static uint64_t w_t0; static uint64_t w_n;
        w_n++;
        const uint64_t now = dt3;
        if (!w_t0) w_t0 = now;
        if (now - w_t0 >= 5000000000ull) {
            const uint64_t a0 = __atomic_exchange_n(&g_draw_ns[0], 0, __ATOMIC_RELAXED);
            const uint64_t a1 = __atomic_exchange_n(&g_draw_ns[1], 0, __ATOMIC_RELAXED);
            const uint64_t a2 = __atomic_exchange_n(&g_draw_ns[2], 0, __ATOMIC_RELAXED);
            const uint64_t d0 = __atomic_exchange_n(&g_desc_ns[0], 0, __ATOMIC_RELAXED);
            const uint64_t d1 = __atomic_exchange_n(&g_desc_ns[1], 0, __ATOMIC_RELAXED);
            const uint64_t d2 = __atomic_exchange_n(&g_desc_ns[2], 0, __ATOMIC_RELAXED);
            const uint64_t d3 = __atomic_exchange_n(&g_desc_ns[3], 0, __ATOMIC_RELAXED);
            const uint64_t dnw = __atomic_exchange_n(&g_desc_nw, 0, __ATOMIC_RELAXED);
            const uint64_t dsets = __atomic_exchange_n(&g_desc_sets, 0, __ATOMIC_RELAXED);
            char b2[320];
            snprintf(b2, sizeof b2, "后端逐笔（微秒/笔，共 %llu 笔）：入口 %.2f（描述符准备 %.2f / 更新 %.2f / 绑定 %.2f / 收尾 %.2f）、状态 %.2f、顶点索引与绘制 %.2f；描述符更新 %.2f 笔/笔（%.1f 套）",
                     (unsigned long long)w_n,
                     (double)(a0 + a1 + a2) / 1000.0 / (double)w_n, (double)d0 / 1000.0 / (double)w_n, (double)d1 / 1000.0 / (double)w_n,
                     (double)d2 / 1000.0 / (double)w_n, (double)d3 / 1000.0 / (double)w_n,
                     (double)a1 / 1000.0 / (double)w_n, (double)a2 / 1000.0 / (double)w_n,
                     (double)dnw / (double)w_n, (double)dsets / (double)w_n);
            nvn_plat_log(NVN_PLAT_LOG_INFO, "draw", b2);
            w_t0 = now; w_n = 0;
        }
    }
}

// 本段未实现的区域：一律粘滞 UNSUPPORTED / 返回 UNSUPPORTED，不空实现。
#define VK_NOT_YET(c, name) set_sticky((c), NVN_GPU_E_UNSUPPORTED, name "：D8 第一段未实现（随后接通道 / 管线）")

static int g_test_no_xfer_bar;   // 测试钩子 nvn_gpu_vk_test_xfer_barrier 置（文件末）
void nvn_gpu_dispatch(nvn_gpu_cmd ch, nvn_gpu_pipe ph, uint32_t x, uint32_t y, uint32_t z, nvn_gpu_buf ind, uint64_t off) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!ind && (!x || !y || !z)) return;   // 0 组：空操作，不录、不置错（对齐 Ryujinx）
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 在通道内"); return; }
    es_reset(c);   // push 常量的范围按管线布局、不分绑定点：保守起见计算派发之后图形这边全部重发（#708）
    if (ph && c->pipe && ph != c->pipe) { set_sticky(c, NVN_GPU_E_ARG, "dispatch 管线与 bind 时的管线不一致"); return; }
    struct nvn_gpu_pipe_s *p = c->pipe;
    if (!p || p->pass) { set_sticky(c, NVN_GPU_E_ORDER, "dispatch 缺计算管线"); return; }
    if (ind && x | y | z) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 与计数不能同时给"); return; }
    if (ind) { if (!buf_range(ind, off, 12)) { set_sticky(c, NVN_GPU_E_ARG, "间接 dispatch 参数越界"); return; } }
    c->pipe = p;
    if (!prepare_bind_layouts(c, 1)) return;
    if (!bind_descriptor_groups(c, p, 1)) return;
    if (c->push_have[NVN_GPU_SH_CS] && p->stage_push_bytes[NVN_GPU_SH_CS])
        vkCmdPushConstants(c->cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           p->stage_push_off[NVN_GPU_SH_CS], p->stage_push_bytes[NVN_GPU_SH_CS],
                           c->push_data[NVN_GPU_SH_CS] + p->stage_push_off[NVN_GPU_SH_CS]);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    // 之前录过缓冲作目标的传输（核心的池镜像同步、缓冲写 / 填 / 拷贝、贴图→缓冲）：契约说之后的读看到新值（nvn_backend.h buf_write），
    // 传输自己只在开头带屏障，这里补一道再派发；连续派发之间不加（NVN 不保证无屏障的派发之间的顺序，游戏屏障另走 nvn_gpu_barrier）
    if (c->xfer_dirty && !g_test_no_xfer_bar) full_barrier(c);
    if (ind) {
        vkCmdDispatchIndirect(c->cb, ind->buffer, off);
        cmd_ref(c, ind, REF_BUF);
    } else {
        vkCmdDispatch(c->cb, x, y, z);
    }
}
// ---------------------------------------------------------------- 贴图 / 视图 / 采样器

static uint32_t fmt_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:   // 打包成一个 32 位字的格式
        return 4;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;   // 压缩 / 打包格式先不算行宽：调用方按 E_UNSUPPORTED 处理
    }
}

static VkImageAspectFlags fmt_aspect(VkFormat f) {
    if (!nvn_vk_format_is_depth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (nvn_vk_format_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkPipelineStageFlags layout_dst_stage(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:   // 通道开头的深度载入与颜色输出同序（任务约定）
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        // 采样只发生在着色器阶段；ALL_GRAPHICS 不含 tessellation 那几个可选位，设备没开该特性也不违规
        return VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}
static VkAccessFlags layout_dst_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;   // 载入要读、绘制要写
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}
static VkPipelineStageFlags layout_src_stage(VkImageLayout l) {   // 离开这个布局：等谁写完
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return layout_dst_stage(l);
    case VK_IMAGE_LAYOUT_GENERAL: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    default: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}
static VkAccessFlags layout_src_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}

// 录一次布局迁移：本段内跟踪，段首一律按 GENERAL 算（见「图像布局」）
static void image_barrier_from(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    b.srcAccessMask = layout_src_access(from);
    b.dstAccessMask = layout_dst_access(to);
    vkCmdPipelineBarrier(c->cb, layout_src_stage(from), layout_dst_stage(to), 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;   // 只做诊断：真正的「本段当前布局」在 c->lay 里
}

// 这张图当前有没有被绑成采样贴图 / 存储图像：采样描述符里声明的 imageLayout 跟着本段布局走，布局一变这些组就作废
static int tex_bound_for_image(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t) {
    for (int st = 0; st < 6; st++)
        for (uint32_t i = 0; i < c->nbinds[st]; i++) if (bind_tex_of(&c->binds[st][i].b) == t) return 1;
    return 0;
}

// 记录线程单线程编码（契约 [提交]）；布局按「本段内」跟踪：段首一律 GENERAL，段末转回 GENERAL。
static void image_barrier(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, VkImageLayout to) {
    struct cmd_lay *e = lay_get(c, t, 0);
    VkImageLayout from = e ? e->cur : VK_IMAGE_LAYOUT_GENERAL;
    if (from == to) return;   // 不用迁：不占表、不录屏障（旧行为下一切本来就都在 GENERAL）
    if (!e) { e = lay_get(c, t, 1); if (!e) { set_sticky(c, NVN_GPU_E_OOM, "布局跟踪表分配失败"); return; } }
    image_barrier_from(c, t, from, to);
    e->cur = to;
    if (c->opt && tex_bound_for_image(c, t)) c->desc_dirty = 0xfu;   // 采样描述符里的 imageLayout 作废，下一笔全重写
}

// 段末：本段动过的图一律转回 GENERAL（段与段之间的静止布局约定，见「图像布局」；只转真的改过的）
static void lay_rest_at_commit(struct nvn_gpu_cmd_s *c) {
    for (uint32_t i = 0; i < c->nlay; i++) {
        if (c->lay[i].cur == VK_IMAGE_LAYOUT_GENERAL) continue;
        image_barrier_from(c, c->lay[i].t, c->lay[i].cur, VK_IMAGE_LAYOUT_GENERAL);
        c->lay[i].cur = VK_IMAGE_LAYOUT_GENERAL;
    }
}

// 压缩格式（块 > 1）：区域可以按整块给到 mip 边缘之外（小于一块的 mip，例如 2×2，游戏照 NVN 给 4×4），
//   只要没超过边缘所在的那一整块，就裁到边（Vulkan 对压缩格式本来就要求区域到边为止）；超过整块照旧非法。
static void copy_region_block_clamp(const struct nvn_gpu_tex_s *t, uint32_t w, uint32_t h, NVNcopyRegion *rr) {
    nvn_fmt_info f;
    if (!nvn_fmt_info_of(t->nvn_format, &f) || (f.bw <= 1 && f.bh <= 1) || rr->xoffset < 0 || rr->yoffset < 0) return;
    uint32_t bw = f.bw > 1 ? f.bw : 1, bh = f.bh > 1 ? f.bh : 1;
    uint32_t aw = (w + bw - 1) / bw * bw, ah = (h + bh - 1) / bh * bh;
    if (rr->width > 0 && (uint32_t)rr->xoffset < w && (uint32_t)rr->xoffset + (uint32_t)rr->width > w &&
        (uint32_t)rr->xoffset + (uint32_t)rr->width <= aw) rr->width = (int)(w - (uint32_t)rr->xoffset);
    if (rr->height > 0 && (uint32_t)rr->yoffset < h && (uint32_t)rr->yoffset + (uint32_t)rr->height > h &&
        (uint32_t)rr->yoffset + (uint32_t)rr->height <= ah) rr->height = (int)(h - (uint32_t)rr->yoffset);
}

static int copy_region(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r_in, VkOffset3D *off, VkExtent3D *ext) {
    uint32_t w = t->w >> level, h = t->h >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    NVNcopyRegion clamped; const NVNcopyRegion *r = r_in;
    if (r_in) { clamped = *r_in; copy_region_block_clamp(t, w, h, &clamped); r = &clamped; }
    if (t->target == NVN_TEXTURE_TARGET_3D) {
        if (r && (r->width > 0 || r->height > 0)) {
            uint32_t rz = (uint32_t)(r->depth > 0 ? r->depth : 1);
            if (r->xoffset < 0 || r->yoffset < 0 ||
                (uint32_t)r->xoffset + (uint32_t)r->width > w ||
                (uint32_t)r->yoffset + (uint32_t)r->height > h) return -1;
            *off = (VkOffset3D){r->xoffset, r->yoffset, r->zoffset};
            *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, rz};
        } else {
            *off = (VkOffset3D){0, 0, r ? r->zoffset : 0};
            *ext = (VkExtent3D){w, h, (uint32_t)(r && r->depth > 0 ? (uint32_t)r->depth : d)};
        }
        // 两个宽高分支都必须落在当前 mip 的深度内；减法避免区间末端溢出。
        if (off->z < 0 || (uint32_t)off->z >= d || ext->depth > d - (uint32_t)off->z) return -1;
        return 0;
    }
    if (r && r->width > 0 && r->height > 0) {
        if (r->xoffset < 0 || r->yoffset < 0 || (uint32_t)r->xoffset + (uint32_t)r->width > w ||
            (uint32_t)r->yoffset + (uint32_t)r->height > h || r->zoffset != 0 || r->depth > 1)
            return -1;
        *off = (VkOffset3D){r->xoffset, r->yoffset, 0};
        *ext = (VkExtent3D){(uint32_t)r->width, (uint32_t)r->height, 1};
    } else {
        *off = (VkOffset3D){0, 0, 0};
        *ext = (VkExtent3D){w, h, 1};
    }
    return 0;
}

static int check_copy_params(const struct nvn_gpu_tex_s *t, uint32_t level, const NVNcopyRegion *r,
                             uint64_t row_bytes, uint64_t image_bytes, uint32_t bpp,
                             VkOffset3D *off, VkExtent3D *ext) {
    if (!t || level >= t->levels) return -1;
    if (copy_region(t, level, r, off, ext)) return -1;
    if (!row_bytes || row_bytes < (uint64_t)ext->width * bpp) return -1;
    if (!image_bytes || image_bytes < row_bytes * ext->height) return -1;
    return 0;
}

// 建贴图（vkCreateImage + 要显存 + 建默认视图）在真机上单张 0.3–2 毫秒；走路/进新场景时会成批出现，
// 所以超过 3 毫秒的单独记一行，用来和「长帧」对时间（见坑记录续 34）。
nvn_gpu_rc nvn_gpu_tex_create(const nvn_gpu_tex_desc *d, nvn_gpu_tex *out, nvn_gpu_err *err) {
    const uint64_t tc_t0 = now_ns();
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (!d) return fail(err, NVN_GPU_E_ARG, "描述为空");
    if (d->host_w || d->host_h) return fail(err, NVN_GPU_E_UNSUPPORTED, "宽屏宿主尺寸（host_w/h）：本段未实现");
    if (d->stencil_view) return fail(err, NVN_GPU_E_UNSUPPORTED, "模板视图：本段未实现");
    uint32_t target = (uint32_t)d->d.target;
    uint32_t depth = d->d.depth ? (uint32_t)d->d.depth : 1;
    uint32_t layers = 1;
    VkImageType itype = VK_IMAGE_TYPE_2D;
    VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateFlags iflags = 0;
    int array_compat = 0;
    switch (target) {
    case NVN_TEXTURE_TARGET_2D:
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: break;
    case NVN_TEXTURE_TARGET_2D_ARRAY:
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NVN_TEXTURE_TARGET_3D:
        itype = VK_IMAGE_TYPE_3D; vtype = VK_IMAGE_VIEW_TYPE_3D;
        array_compat = 1;
        iflags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        break;
    case NVN_TEXTURE_TARGET_CUBEMAP:
        layers = 6; vtype = VK_IMAGE_VIEW_TYPE_CUBE;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        if (!g.dev->core_features.imageCubeArray)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "立方体数组需要 imageCubeArray 设备特性（未启用）");
        if (!depth || depth % 6) return fail(err, NVN_GPU_E_ARG, "立方体数组 depth=%u 必须是 6 的倍数", depth);
        layers = depth; vtype = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        iflags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    default:
        return fail(err, NVN_GPU_E_UNSUPPORTED, "target %u：本段只实现 2D / 2D 数组 / 立方体 / 立方体数组 / 3D", target);
    }
    VkFormat fmt = nvn_vk_tex_format((uint32_t)d->d.format);
    if (fmt == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_UNSUPPORTED, "NVN 格式 0x%x 无 Vulkan 映射", d->d.format);
    int bc_decoded = d->d.format >= 0x41 && d->d.format <= 0x50 &&
                     ((g.dev->bc_decode_mask >> ((uint32_t)d->d.format - 0x41)) & 1u);
    VkFormat sample_fmt = fmt;
    // BC4 3D 按 z 片解码到私有 2D 图；2D → 3D 拷贝依赖 Vulkan 1.1 的 maintenance1 语义。
    if (bc_decoded && (!nvn_vk_bcdec_route((uint32_t)d->d.format, NULL, &sample_fmt, &fmt, NULL, NULL) ||
                       (itype != VK_IMAGE_TYPE_2D && !(itype == VK_IMAGE_TYPE_3D && d->d.format == NVN_FORMAT_RGTC1_UNORM &&
                                                    g.dev->properties.apiVersion >= VK_API_VERSION_1_1)) ||
                       (d->usage & (NVN_GPU_TEX_RENDER | NVN_GPU_TEX_STORAGE))))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "BC format 0x%x decode: 不支持此用途或维度", d->d.format);
    if (!d->d.width || !d->d.height) return fail(err, NVN_GPU_E_ARG, "尺寸为 0");
    uint32_t levels = d->d.levels ? (uint32_t)d->d.levels : 1;
    uint32_t samples = d->d.samples > 1 ? (uint32_t)d->d.samples : 1;
    if (bc_decoded && samples != 1) return fail(err, NVN_GPU_E_UNSUPPORTED, "BC 计算解码不支持多重采样");

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d->usage & NVN_GPU_TEX_SAMPLED) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d->usage & NVN_GPU_TEX_RENDER) {
        usage |= nvn_vk_format_is_depth(fmt) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (d->usage & NVN_GPU_TEX_STORAGE) usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    // 在 vkCreateImage 前按真实格式/usage 查询样本数；不把非法计数送到驱动或校验层。
    int ms_target = target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
    if (ms_target || samples > 1) {
        if (d->d.samples <= 1 || samples > 64 || (samples & (samples - 1)) || levels != 1 || itype != VK_IMAGE_TYPE_2D ||
            ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !g.dev->core_features.shaderStorageImageMultisample) ||
            (d->d.samples > 1 && (target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)))
            return fail(err, NVN_GPU_E_UNSUPPORTED, "多重采样计数/级数/目标不支持");
        VkImageFormatProperties ip = {0};
        VkImageCreateFlags flags = iflags | ((d->usage & NVN_GPU_TEX_VIEW_FMT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
        if (vkGetPhysicalDeviceImageFormatProperties(g.phys, fmt, itype, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ip) != VK_SUCCESS ||
            !(ip.sampleCounts & samples) || (uint32_t)d->d.width > ip.maxExtent.width || (uint32_t)d->d.height > ip.maxExtent.height || layers > ip.maxArrayLayers)
            return fail(err, NVN_GPU_E_UNSUPPORTED, "设备不支持该格式/usage 的 %u 重采样贴图", samples);
    }

    struct nvn_gpu_tex_s *t = calloc(1, sizeof *t);
    if (!t) return fail(err, NVN_GPU_E_OOM, "贴图结构分配失败");
    t->format = fmt;
    t->sample_format = sample_fmt;
    t->bc_decoded = bc_decoded;
    t->aspect = fmt_aspect(fmt);
    t->w = d->d.width;
    t->h = d->d.height;
    t->levels = levels;
    t->layers = layers;
    t->depth = depth;
    t->target = target;
    t->array_compatible = array_compat;
    t->samples = samples;
    t->nvn_format = (uint32_t)d->d.format;
    t->view_fmt = (d->usage & NVN_GPU_TEX_VIEW_FMT) != 0;
    t->swz = d->swz;
    t->layout = VK_IMAGE_LAYOUT_GENERAL;   // 实际转换在创建后的第一次提交里（待转表）

    VkImageCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = itype;
    ci.format = fmt;
    ci.extent = (VkExtent3D){d->d.width, d->d.height, itype == VK_IMAGE_TYPE_3D ? depth : 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.flags = iflags;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (t->view_fmt || sample_fmt != fmt) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    LIFE_T0;
    if (vkCreateImage(g.d, &ci, NULL, &t->image) != VK_SUCCESS) {
        free(t);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImage 拒绝（格式 0x%x / usage 0x%x / %ux%u）", d->d.format, usage, d->d.width, d->d.height);
    }
    LIFE_ADD(0);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.d, t->image, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    t->mem_size = req.size; t->mem_type = type;
    if (type != UINT32_MAX && texmem_on()) t->memory = texmem_take(req.size, type, &t->mem_size);
    if (type == UINT32_MAX || (!t->memory && vkAllocateMemory(g.d, &ai, NULL, &t->memory) != VK_SUCCESS) ||
        vkBindImageMemory(g.d, t->image, t->memory, 0) != VK_SUCCESS) {
        if (t->memory) vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "贴图显存分配失败（%ux%u，%u 级）", d->d.width, d->d.height, levels);
    }
    LIFE_ADD(1);
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = vtype;
    vi.format = sample_fmt;
    vi.subresourceRange = (VkImageSubresourceRange){(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, 0, levels, 0, itype == VK_IMAGE_TYPE_3D ? 1 : layers};   // 深度模板格式：采样只取深度面
    if (vkCreateImageView(g.d, &vi, NULL, &t->view) != VK_SUCCESS) {
        vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "默认贴图视图创建失败");
    }
    LIFE_ADD(2);
    pthread_mutex_lock(&g_mu);
    if (!newimg_add_locked(t)) {
        pthread_mutex_unlock(&g_mu);
        pc_drop_view(t->view);
        vkDestroyImageView(g.d, t->view, NULL);
        vkFreeMemory(g.d, t->memory, NULL);
        vkDestroyImage(g.d, t->image, NULL);
        free(t);
        return fail(err, NVN_GPU_E_OOM, "待转布局表分配失败");
    }
    t->all_next = g.texs;
    g.texs = t;
    pthread_mutex_unlock(&g_mu);
    *out = t;
    { const uint64_t dt = now_ns() - tc_t0;   // 建贴图超过 3 毫秒单独记一行（走路 / 进新场景时会成批出现；nvk_hitch_log 开时才记）
      static int slowt = -1; if (slowt < 0) slowt = nvn_plat_flag("nvk_hitch_log", 0) != 0;
      if (slowt && dt > 3000000ull) { char b[160]; snprintf(b, sizeof b, "建贴图 %.1f 毫秒（%ux%u）", (double)dt / 1e6, d->d.width, d->d.height); nvn_plat_log(NVN_PLAT_LOG_INFO, "tex", b); } }
    return rc(NVN_GPU_OK);
}

void nvn_gpu_tex_info_get(nvn_gpu_tex th, nvn_gpu_tex_info *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    out->w = t->w;
    out->h = t->h;
    out->depth = t->depth;
    out->levels = t->levels;
    out->layers = t->layers;
    out->samples = t->samples;
    out->nvn_format = t->nvn_format;
    out->fmt_flags = nvn_gpu_format_caps(t->nvn_format);
}

void nvn_gpu_tex_release(nvn_gpu_tex th) {
    struct nvn_gpu_tex_s *t = th;
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    if (!t->released) {
        t->released = 1;
        if (t->refs == 0) destroy_tex_locked(&g, t);
    }
    pthread_mutex_unlock(&g_mu);
}

// NVN 里第 4 通道读作 1 的格式（RGBX / BGRX / RGB5 / BGR5 / RGB_DXT1）：这些格式的存储没有 alpha，
// 分量 a 由硬件供给常数 1。宿主格式是同位宽的 RGBA（Vulkan 没有 RGBX），直接采样会把 X 位当 alpha 读出来
// （0100F2C0115B6000 的光照合成逐层变暗就是这么来的：片元 alpha 读成 0 附近的垃圾值，目标因子 SRC_ALPHA 把已有光照乘小）。
// 与 Metal 侧同一张表（graphics/nvn/gfx/tex/nvn_fmt_mtl.c 的 noalpha：nvn_fmt_to_mtl 把 a 解析成 ONE）。
static int nvn_fmt_alpha_reads_one(uint32_t f) {
    switch (f) {
    case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_RGB_DXT1: case NVN_FORMAT_RGB_DXT1_SRGB: case NVN_FORMAT_RGB5:
    case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
    case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI: case NVN_FORMAT_RGBX16I:
    case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
    case NVN_FORMAT_BGR5: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
        return 1;
    default: return 0;
    }
}

nvn_gpu_rc nvn_gpu_view_create(nvn_gpu_tex th, const view_t *v, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    struct nvn_gpu_tex_s *t = th;
    if (!t) return fail(err, NVN_GPU_E_ARG, "贴图为空");
    uint32_t base_level = 0, levels = t->levels, base_layer = 0, layers = t->layers;
    VkFormat fmt = t->sample_format;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    if (v) {
        if (v->set_levels) { base_level = v->base_level; levels = v->nlevels ? v->nlevels : 1; }
        if (v->set_layers) { base_layer = v->min_layer; layers = v->nlayers ? v->nlayers : 1; }
        if (v->set_format && (uint32_t)v->format != t->nvn_format) {
            if (t->bc_decoded) return fail(err, NVN_GPU_E_UNSUPPORTED, "BC 解码贴图的不同格式视图未实现");
            VkFormat of = nvn_vk_tex_format((uint32_t)v->format);
            if (of == VK_FORMAT_UNDEFINED) return fail(err, NVN_GPU_E_ARG, "视图格式 0x%x 无映射", v->format);
            fmt = of;
        }
    }
    struct nvn_vk_texview_plan plan;
    nvn_vk_texview_plan_make(t->target, t->depth, t->layers, t->levels, v, 0, t->array_compatible, &plan);
    if (!plan.ok) return fail(err, NVN_GPU_E_ARG, "%s", plan.why);
    base_level = plan.base_level; levels = plan.nlevels;
    base_layer = plan.base_layer; layers = plan.nlayers; type = plan.type;
    if (fmt != t->sample_format && !t->view_fmt)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "不同格式的视图需要创建时声明 NVN_GPU_TEX_VIEW_FMT");

    struct nvn_gpu_view_s *o = calloc(1, sizeof *o);
    if (!o) return fail(err, NVN_GPU_E_OOM, "视图结构分配失败");
    o->nvn_format = v && v->set_format ? (uint32_t)v->format : t->nvn_format;
    o->format = fmt;
    VkImageViewCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = t->image;
    ci.viewType = type;
    ci.format = fmt;
    // 深度模板格式：采样视图只能取一个面（两个面都取的视图不能绑进描述符）。视图设了深度模板模式按它，否则取深度面
    //（对照 nvn_mtl.m @6602 want_st；存储自带「模板视图」的贴图本后端建不出来，见 tex_create）。当附件时另用附件形态（两个面都在）。
    o->aspect = t->aspect;
    if ((t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        o->aspect = v && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    o->type = type;
    o->att_layer = v && v->set_layers ? v->min_layer : 0;
    ci.subresourceRange = (VkImageSubresourceRange){o->aspect, base_level, levels, base_layer, layers};
    // 通道映射：视图自己设了就用视图的，否则用存储自带的（与 nvn_mtl.m:6546 同序，是取代不是叠加）。
    // NVN 的取值 0 零 / 1 一 / 2 R / 3 G / 4 B / 5 A，其它值按该通道恒等。
    {
        uint8_t sw[4] = {2, 3, 4, 5};
        if (v && v->set_swizzle) memcpy(sw, v->swz, 4);
        else if (t->swz) for (int ch = 0; ch < 4; ch++) sw[ch] = (uint8_t)(t->swz >> (3 * ch) & 7);
        VkComponentSwizzle *comp[4] = {&ci.components.r, &ci.components.g, &ci.components.b, &ci.components.a};
        int a_one = nvn_fmt_alpha_reads_one(o->nvn_format);   // 没有 alpha 的格式：不管映射到哪一档，读到 a 都是 1
        for (int ch = 0; ch < 4; ch++) {
            if (sw[ch] > 5) sw[ch] = (uint8_t)(2 + ch);
            if (a_one && sw[ch] == 5) sw[ch] = 1;
            *comp[ch] = sw[ch] == 0 ? VK_COMPONENT_SWIZZLE_ZERO : sw[ch] == 1 ? VK_COMPONENT_SWIZZLE_ONE : (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + (sw[ch] - 2));
            if (sw[ch] != 2 + ch) o->swizzled = 1;
        }
        if (!o->swizzled) memset(&ci.components, 0, sizeof ci.components);   // 恒等：留 IDENTITY，附件可用
    }
    OC(OC_VIEW);
    if (vkCreateImageView(g.d, &ci, NULL, &o->view) != VK_SUCCESS) {
        free(o);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateImageView 拒绝（格式 / 范围）");
    }
    o->tex = t;
    o->base_level = base_level;
    o->levels = levels;
    o->base_layer = base_layer;
    o->layers = layers;
    pthread_mutex_lock(&g_mu);
    t->refs++;   // 视图让贴图活到视图 release
    o->all_next = g.views;
    g.views = o;
    pthread_mutex_unlock(&g_mu);
    *out = o;
    return rc(NVN_GPU_OK);
}

nvn_gpu_rc nvn_gpu_view_create_buf(nvn_gpu_buf b, uint64_t off, uint64_t bytes, uint32_t fmt, nvn_gpu_view *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    nvn_fmt_info info;
    VkFormat f = nvn_vk_format(fmt);
    if (f == VK_FORMAT_UNDEFINED || !nvn_fmt_info_of(fmt, &info) ||
        info.bw != 1 || info.bh != 1 || !info.bytes)
        return fail(err, NVN_GPU_E_UNSUPPORTED, "缓冲视图格式 0x%x 不支持", fmt);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &fp);
    VkFormatFeatureFlags features = fp.bufferFeatures &
        (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT);
    uint32_t caps = nvn_gpu_format_caps(fmt);
    // 底层 VkBuffer 同时声明两种 texel 用途；视图必须满足两项格式特性。
    if (features != (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持缓冲声明的全部 texel 用途", fmt);
    if (!((features & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_SAMPLE)) &&
        !((features & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) && (caps & NVN_GPU_FMT_STORAGE)))
        return fail(err, NVN_GPU_E_UNSUPPORTED, "格式 0x%x 不支持纹素缓冲", fmt);
    uint64_t align = g.dev->properties.limits.minTexelBufferOffsetAlignment;
    if (!bytes || bytes % info.bytes || (align && off % align) ||
        bytes / info.bytes > g.dev->properties.limits.maxTexelBufferElements)
        return fail(err, NVN_GPU_E_ARG, "缓冲视图范围 / 对齐非法");
    struct nvn_gpu_view_s *v = calloc(1, sizeof *v);
    if (!v) return fail(err, NVN_GPU_E_OOM, "缓冲视图结构分配失败");
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_buf_s *found = g.bufs;
    while (found && found != b) found = found->all_next;
    if (g.closing || !found || found->released || !buf_range(found, off, bytes)) {
        int closing = g.closing;
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, closing ? NVN_GPU_E_STOPPED : NVN_GPU_E_ARG, "缓冲无效或视图范围越界");
    }
    VkBufferViewCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    ci.buffer = found->buffer; ci.format = f; ci.offset = off; ci.range = bytes;
    VkResult result = vkCreateBufferView(g.d, &ci, NULL, &v->buffer_view);
    if (result != VK_SUCCESS) {
        pthread_mutex_unlock(&g_mu);
        free(v);
        return fail(err, result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                         ? NVN_GPU_E_OOM : NVN_GPU_E_UNSUPPORTED, "vkCreateBufferView 失败（%d）", result);
    }
    found->refs++;
    v->buf = found; v->nvn_format = fmt; v->buffer_format = f; v->buffer_features = features; v->buffer_caps = caps;
    v->all_next = g.views; g.views = v;
    pthread_mutex_unlock(&g_mu);
    *out = v;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_view_release(nvn_gpu_view vh) {
    struct nvn_gpu_view_s *v = vh;
    if (!v) return;
    pthread_mutex_lock(&g_mu);
    if (!v->released) {
        v->released = 1;
        if (v->refs == 0) destroy_view_locked(&g, v);
    }
    pthread_mutex_unlock(&g_mu);
}

static VkSamplerAddressMode samp_wrap(uint8_t w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP:
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER:
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkCompareOp samp_compare(uint8_t m) {
    switch (m) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

nvn_gpu_rc nvn_gpu_samp_create(const samp_par *p, nvn_gpu_samp *out, nvn_gpu_err *err) {
    if (!session_accepting()) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭 / 未打开");
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!p) return fail(err, NVN_GPU_E_ARG, "samp_par 为空");
    struct nvn_gpu_samp_s *s = calloc(1, sizeof *s);
    if (!s) return fail(err, NVN_GPU_E_OOM, "采样器结构分配失败");
    VkSamplerCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    switch (p->minf) {
    case NVN_MIN_FILTER_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; break;
    case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; break;
    default: free(s); return fail(err, NVN_GPU_E_ARG, "NVN 最小过滤 %u 无映射", p->minf);
    }
    ci.addressModeU = samp_wrap(p->ws);
    ci.addressModeV = samp_wrap(p->wt);
    ci.addressModeW = samp_wrap(p->wr);
    ci.mipLodBias = p->lod_bias;
    ci.minLod = p->lod_min;
    ci.maxLod = p->lod_max;
    if (p->aniso > 1.0f) {
        if (!g.dev->core_features.samplerAnisotropy) {
            free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "请求各向异性 %.2f，但设备未启用 samplerAnisotropy", p->aniso);
        }
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = p->aniso;
    } else ci.maxAnisotropy = 1.0f;
    if (p->cmp_mode != NVN_COMPARE_MODE_NONE) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = samp_compare(p->cmp_func);
    }
    int white = p->border[0] == 1 && p->border[1] == 1 && p->border[2] == 1 && p->border[3] == 1;
    int transparent = p->border[0] == 0 && p->border[1] == 0 && p->border[2] == 0 && p->border[3] == 0;
    ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : transparent ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(g.d, &ci, NULL, &s->sampler) != VK_SUCCESS) {
        free(s);
        return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateSampler 拒绝（可能缺少 samplerAnisotropy）");
    }
    {   // 整数格式视图用的那一个：过滤全改最近点、关各向异性、边界色换整数的同名值
        VkSamplerCreateInfo ii = ci;
        ii.magFilter = ii.minFilter = VK_FILTER_NEAREST; ii.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ii.anisotropyEnable = VK_FALSE; ii.maxAnisotropy = 1.0f;
        ii.borderColor = white ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : transparent ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        if (vkCreateSampler(g.d, &ii, NULL, &s->int_sampler) != VK_SUCCESS) {
            vkDestroySampler(g.d, s->sampler, NULL); free(s);
            return fail(err, NVN_GPU_E_UNSUPPORTED, "vkCreateSampler 拒绝（整数格式用的最近点采样器）");
        }
    }
    pthread_mutex_lock(&g_mu);
    s->all_next = g.samps;
    g.samps = s;
    pthread_mutex_unlock(&g_mu);
    *out = s;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_samp_release(nvn_gpu_samp sh) {
    struct nvn_gpu_samp_s *s = sh;
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->released) {
        s->released = 1;
        if (s->refs == 0) destroy_samp_locked(&g, s);
    }
    pthread_mutex_unlock(&g_mu);
}

static void bc_readback(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes);   // 见 bc_upload 前
static void bc_copy_from_buf(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                             struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes);   // 见 bc_upload 后
// 线性内容上传 / 缓冲 ↔ 贴图拷贝。全部在通道外，按 GPU 顺序执行。
// depth_f32：#398 D24 换 D32 存储的贴图，缓冲里已是换算好的 D32 深度面（每纹素 4 字节 float），只拷深度面。
static void copy_buf_tex_impl(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                              nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r, int depth_f32) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    if (t && t->bc_decoded) {   // 压缩字节进现解贴图：在段里解码（#697），不直拷
        if (!b) { set_sticky(c, NVN_GPU_E_ARG, "缓冲为空"); return; }
        NVNcopyRegion r3;
        if (t->target == NVN_TEXTURE_TARGET_3D) {   // 与下面直拷同一口径：3D 贴图的 layer 是切片号（#697 Mali：32×32×32 的 BC 贴图按 layer 1 进来被判「层越界」）
            if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
            memset(&r3, 0, sizeof r3);
            if (r) r3 = *r;
            if (r3.depth <= 0) r3.depth = 1;
            r3.zoffset = (int)layer;
            r = &r3; layer = 0;
        }
        bc_copy_from_buf(c, t, level, layer, r, b, off, row_bytes, image_bytes);
        return;
    }
    if (t && !depth_f32 && nvn_vk_format_substituted(t->nvn_format)) {   // 格式能力已报 NVN_GPU_FMT_SUBST：调用方给的是 D24 字节
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "替换存储格式（D24 换 D32）的缓冲字节不能直接拷（0x%x），走 nvn_gpu_tex_upload", t->nvn_format);
        return;
    }
    uint32_t bpp = depth_f32 ? 4 : t ? fmt_bpp(t->format) : 0;
    // 块信息：压缩格式一行是一排块（blk_w x blk_h 纹素、blk 字节）；没列进 fmt_bpp 的非压缩格式按 NVN 格式表的每纹素字节数
    uint32_t blk_w = 1, blk_h = 1, blk = bpp;
    if (t && !depth_f32 && !bpp) {
        nvn_fmt_info fi;
        if (nvn_fmt_info_of(t->nvn_format, &fi) && !nvn_fmt_expand_target(t->nvn_format, NULL, NULL)) { blk_w = fi.bw; blk_h = fi.bh; blk = fi.bytes; bpp = blk; }
    }
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        (blk_w == 1 && blk_h == 1 ? check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)
                            : (level >= t->levels || copy_region(t, level, r, &o, &e) || !row_bytes ||
                               row_bytes < (uint64_t)((e.width + blk_w - 1) / blk_w) * blk || image_bytes < row_bytes * ((e.height + blk_h - 1) / blk_h) ||
                               (o.x % (int32_t)blk_w) || (o.y % (int32_t)blk_h)))) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_buf_tex 参数非法（格式 0x%x 级 %u：越界 / 行宽 %llu / 总 %llu / 偏移 %llu 未 4 对齐）", t ? t->nvn_format : 0, level,
                   (unsigned long long)row_bytes, (unsigned long long)image_bytes, (unsigned long long)off);
        return;
    }
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / blk) * blk_w;   // 以纹素计；压缩格式是块数 × 块宽
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? ((e.height + blk_h - 1) / blk_h) * blk_h : (uint32_t)(image_bytes / row_bytes) * blk_h;
    bc.imageSubresource = (VkImageSubresourceLayers){depth_f32 ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, level,
                                                     t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyBufferToImage(c->cb, b->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);   // 之后按采样 / 存储使用；本段统一用 GENERAL
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
}

void nvn_gpu_copy_buf_tex(nvn_gpu_cmd ch, nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes,
                          nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r) {
    copy_buf_tex_impl(ch, bh, off, row_bytes, image_bytes, th, level, layer, r, 0);
}

void nvn_gpu_copy_tex_buf(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                          nvn_gpu_buf bh, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *t = th;
    struct nvn_gpu_buf_s *b = bh;
    if (t && t->bc_decoded) { bc_readback(c, t, level, layer, r, b, off, row_bytes, image_bytes); return; }   // 按上传时留下的压缩原字节回读
    nvn_fmt_info fi;
    if (t && nvn_fmt_info_of(t->nvn_format, &fi) && fi.expand) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "展开格式回读未实现（0x%x）", t->nvn_format);
        return;
    }
    if (t && nvn_vk_format_substituted(t->nvn_format)) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "替换存储格式（D24 换 D32）按字节回读未实现（0x%x）", t->nvn_format);
        return;
    }
    uint32_t bpp = t ? fmt_bpp(t->format) : 0;
    // 块信息（同 copy_buf_tex_impl）：压缩格式一行是一排块（blk_w x blk_h 纹素、blk 字节），区域按纹素给、偏移要落在块边上；
    // 没列进 fmt_bpp 的非压缩格式按 NVN 格式表的每纹素字节数。原先这里对压缩格式一律拒绝（核心把块压缩贴图逐字节拷进影子要用它）。
    uint32_t blk_w = 1, blk_h = 1, blk = bpp;
    if (t && !bpp) {
        nvn_fmt_info bi;
        if (nvn_fmt_info_of(t->nvn_format, &bi) && !nvn_fmt_expand_target(t->nvn_format, NULL, NULL)) { blk_w = bi.bw; blk_h = bi.bh; blk = bi.bytes; bpp = blk; }
    }
    VkOffset3D o;
    VkExtent3D e;
    NVNcopyRegion rr;
    if (t && t->target == NVN_TEXTURE_TARGET_3D) {
        if (layer >= t->depth) { set_sticky(c, NVN_GPU_E_ARG, "3D 贴图 slice %u 越界（depth=%u）", layer, t->depth); return; }
        memset(&rr, 0, sizeof rr);
        if (r) rr = *r;
        if (rr.depth <= 0) rr.depth = 1;
        rr.zoffset = (int)layer;
        r = &rr;
    }
    if (!t || !b || !bpp || (t->target != NVN_TEXTURE_TARGET_3D && layer >= t->layers) || (off & 3) ||
        !buf_range(b, off, image_bytes) ||
        (blk_w == 1 && blk_h == 1 ? check_copy_params(t, level, r, row_bytes, image_bytes, bpp, &o, &e)
                            : (level >= t->levels || copy_region(t, level, r, &o, &e) || !row_bytes || (off % blk) ||
                               row_bytes < (uint64_t)((e.width + blk_w - 1) / blk_w) * blk || image_bytes < row_bytes * ((e.height + blk_h - 1) / blk_h) ||
                               (o.x % (int32_t)blk_w) || (o.y % (int32_t)blk_h)))) {
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex_buf 参数非法（格式 0x%x 级 %u：越界 / 行宽 %llu / 总 %llu / 偏移 %llu 没对齐）", t ? t->nvn_format : 0, level,
                   (unsigned long long)row_bytes, (unsigned long long)image_bytes, (unsigned long long)off);
        return;
    }
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc;
    memset(&bc, 0, sizeof bc);
    bc.bufferOffset = off;
    bc.bufferRowLength = (uint32_t)(row_bytes / blk) * blk_w;   // 以纹素计；压缩格式是块数 × 块宽
    bc.bufferImageHeight = t->target == NVN_TEXTURE_TARGET_3D ? ((e.height + blk_h - 1) / blk_h) * blk_h : (uint32_t)(image_bytes / row_bytes) * blk_h;   // 块压缩要是块高的整数倍（同 copy_buf_tex_impl）；非压缩 blk_h = 1，原样 e.height
    bc.imageSubresource = (VkImageSubresourceLayers){t->aspect, level, t->target == NVN_TEXTURE_TARGET_3D ? 0 : layer, 1};
    bc.imageOffset = o;
    bc.imageExtent = e;
    vkCmdCopyImageToBuffer(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer, 1, &bc);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
    cmd_ref(c, b, REF_BUF);
}

// 压缩图的偏移按块对齐；尺寸若不足一块，只允许落在对应 mip 的边缘。
static int copy_block_region_ok(const struct nvn_gpu_tex_s *t, uint32_t level, const VkOffset3D *o,
                                const VkExtent3D *e, const nvn_fmt_info *fi) {
    uint32_t w = t->w >> level, h = t->h >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    return (uint32_t)o->x % fi->bw == 0 && (uint32_t)o->y % fi->bh == 0 &&
           (e->width % fi->bw == 0 || (uint64_t)(uint32_t)o->x + e->width == w) &&
           (e->height % fi->bh == 0 || (uint64_t)(uint32_t)o->y + e->height == h);
}

void nvn_gpu_copy_tex(nvn_gpu_cmd ch, nvn_gpu_tex sh, uint32_t slevel, uint32_t slayer, const NVNcopyRegion *sr,
                      nvn_gpu_tex dh, uint32_t dlevel, uint32_t dlayer, const NVNcopyRegion *dr) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_tex_s *s = sh, *d = dh;
    // 两边同一 VkFormat 才拷；D32S8 不在 fmt_bpp 表里（每纹素两个面），同格式深度模板整面拷贝合法，
    // 一并放行（#398：D24S8 换成 D32S8 后贴图间拷贝照旧可用）。
    nvn_fmt_info si = {0}, di = {0};
    const int compressed = s && d && s->format == d->format && !s->bc_decoded && !d->bc_decoded &&
        nvn_vk_format_is_compressed(s->format) && nvn_fmt_info_of(s->nvn_format, &si) &&
        nvn_fmt_info_of(d->nvn_format, &di) && si.bw > 1 && si.bh > 1 &&
        si.bw == di.bw && si.bh == di.bh && si.bytes == di.bytes;
    const int fmt_ok = s && d && s->format == d->format &&
        (fmt_bpp(s->format) || nvn_vk_format_is_depth(s->format) || compressed);
    VkOffset3D so, dof;
    VkExtent3D se, de;
    if (!fmt_ok || slayer >= s->layers || dlayer >= d->layers ||
        slevel >= s->levels || dlevel >= d->levels || slevel >= 32 || dlevel >= 32 ||
        copy_region(s, slevel, sr, &so, &se) || copy_region(d, dlevel, dr, &dof, &de) ||
        se.width != de.width || se.height != de.height || se.depth != de.depth ||
        (compressed && (!copy_block_region_ok(s, slevel, &so, &se, &si) ||
                        !copy_block_region_ok(d, dlevel, &dof, &de, &di)))) {
        // 把是哪一项不合法说清楚（#690：真机上只看到这一句，定不了位）：两边的格式、级 / 层与各自的层数、图的尺寸与级数、两个区域
        const NVNcopyRegion z = {0, 0, 0, 0, 0, 0}; const NVNcopyRegion *a = sr ? sr : &z, *b = dr ? dr : &z;
        set_sticky(c, NVN_GPU_E_ARG, "copy_tex 参数非法：源 格式 %d %ux%ux%u 级 %u/%u 层 %u/%u 区域 %d,%d,%d %dx%dx%d%s；目标 格式 %d %ux%ux%u 级 %u/%u 层 %u/%u 区域 %d,%d,%d %dx%dx%d%s",
                   s ? (int)s->format : -1, s ? s->w : 0, s ? s->h : 0, s ? s->depth : 0, slevel, s ? s->levels : 0, slayer, s ? s->layers : 0, a->xoffset, a->yoffset, a->zoffset, a->width, a->height, a->depth, sr ? "" : "（整级）",
                   d ? (int)d->format : -1, d ? d->w : 0, d ? d->h : 0, d ? d->depth : 0, dlevel, d ? d->levels : 0, dlayer, d ? d->layers : 0, b->xoffset, b->yoffset, b->zoffset, b->width, b->height, b->depth, dr ? "" : "（整级）");
        return;
    }
    full_barrier(c);
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic;
    memset(&ic, 0, sizeof ic);
    ic.srcSubresource = (VkImageSubresourceLayers){s->aspect, slevel, slayer, 1};
    ic.srcOffset = so;
    ic.dstSubresource = (VkImageSubresourceLayers){d->aspect, dlevel, dlayer, 1};
    ic.dstOffset = dof;
    ic.extent = se;
    vkCmdCopyImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, s, REF_TEX);
    cmd_ref(c, d, REF_TEX);
}

void nvn_gpu_clear_view(nvn_gpu_cmd ch, nvn_gpu_view vh, const float rgba[4], const uint32_t u4[4], int is_int) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    struct nvn_gpu_view_s *v = vh;
    if (!v) { set_sticky(c, NVN_GPU_E_ARG, "clear_view 视图为空"); return; }
    struct nvn_gpu_tex_s *t = v->tex;
    if (v->nvn_format != t->nvn_format) {
        // vkCmdClearColorImage 按 image 格式解释 clear 值，无法按 view 的覆盖格式生效
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "clear_view 不支持视图覆盖格式");
        return;
    }
    if (!(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        nvn_fmt_info fi;
        if (nvn_fmt_info_of(v->nvn_format, &fi)) {
            int fmt_int = fi.kind == NVN_FMT_UINT || fi.kind == NVN_FMT_SINT;
            if (fmt_int != (is_int != 0)) {
                set_sticky(c, NVN_GPU_E_ARG, "clear_view 的 is_int 与贴图格式不符");
                return;
            }
        }
    }
    VkImageSubresourceRange range = {t->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    full_barrier(c);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue ds;
        ds.depth = rgba ? rgba[0] : 1.0f;
        ds.stencil = 0;
        vkCmdClearDepthStencilImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ds, 1, &range);
    } else {
        VkClearColorValue col;
        memset(&col, 0, sizeof col);
        for (int i = 0; i < 4; i++) {
            if (is_int) col.uint32[i] = u4 ? u4[i] : 0;
            else col.float32[i] = rgba ? rgba[i] : 0.0f;
        }
        vkCmdClearColorImage(c->cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
    }
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, t, REF_TEX);
}

// 同步解码只写私有临时图：目标的修改仍由当前段在指定位置执行，abort 不改目标。
// 现解贴图（bc_decoded）的压缩原数据影子：显存里存的是解码后的格式，游戏按压缩字节回读（贴图 → 缓冲拷贝）时要的是原字节。
// 这种贴图不能当渲染目标、不能当存储图像，也不接受压缩字节直拷（见 tex_create / copy_buf_tex_impl），内容只来自 bc_upload，
// 所以在上传时留一份原字节就是准确的回读来源，不用重新压缩。影子是一块 GPU 缓冲，写它和读它都录在同一条命令流里：
// 和贴图本身一样按 GPU 顺序生效，命令被放弃（abort）时一起作废。布局：逐级相接；每级内按层（3D 按该级的切片）相接，每层按块行紧排。
static int bc_shadow_layout(const struct nvn_gpu_tex_s *t, uint32_t bb, uint32_t level,
                            uint64_t *level_off, uint64_t *row_pitch, uint64_t *slice_pitch, uint32_t *slices, uint64_t *total) {
    uint64_t at = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = t->w >> l, h = t->h >> l, d = t->target == NVN_TEXTURE_TARGET_3D ? t->depth >> l : t->layers;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!d) d = 1;
        uint64_t row = (uint64_t)((w + 3) / 4) * bb, slice = row * ((h + 3) / 4);
        if (l == level) { *level_off = at; *row_pitch = row; *slice_pitch = slice; *slices = d; }
        if (slice && d > (UINT64_MAX - at) / slice) return 0;
        at += slice * d;
    }
    *total = at;
    return level < t->levels;
}

// 现解贴图的贴图 → 缓冲拷贝：从压缩影子取原字节，按调用方的行距写进目标缓冲（在 GPU 流里按序：暂存区 + 拷贝）。
// 没上传过的区域回零（原机上是未定义内容）。3D 贴图按 copy_tex_buf 的约定 layer 是切片、区域深度为 1。
static int bc_shadow_ensure(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint64_t total);
static void bc_readback(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        struct nvn_gpu_buf_s *b, uint64_t off, uint64_t row_bytes, uint64_t image_bytes) {
    VkOffset3D o; VkExtent3D e; int bb = 0; NVNcopyRegion rr;
    int is3d = t->target == NVN_TEXTURE_TARGET_3D;
    if (is3d) { memset(&rr, 0, sizeof rr); if (r) rr = *r; if (rr.depth > 1) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "BC 解码贴图回读一次只读一个切片"); return; }
                rr.depth = 1; rr.zoffset = (int)layer; r = &rr; }
    if (!b || level >= t->levels || (!is3d && layer >= t->layers) || copy_region(t, level, r, &o, &e) || !e.width || !e.height ||
        !nvn_vk_bcdec_route(t->nvn_format, NULL, NULL, NULL, NULL, &bb) || bb <= 0) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：子资源或区域非法"); return;
    }
    uint32_t mw = t->w >> level, mh = t->h >> level; if (!mw) mw = 1; if (!mh) mh = 1;
    if ((o.x & 3) || (o.y & 3) || ((e.width & 3) && (uint32_t)o.x + e.width != mw) || ((e.height & 3) && (uint32_t)o.y + e.height != mh)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：区域未按压缩块对齐"); return;
    }
    uint64_t bw = ((uint64_t)e.width + 3) / 4, bh = ((uint64_t)e.height + 3) / 4, packed_row = bw * (uint32_t)bb;
    if (row_bytes < packed_row || (bh > 1 && row_bytes > (UINT64_MAX - packed_row) / (bh - 1)) ||
        image_bytes < (bh - 1) * row_bytes + packed_row || !buf_range(b, off, (bh - 1) * row_bytes + packed_row)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：目标行距 / 长度非法"); return;
    }
    uint64_t loff = 0, rp = 0, sp = 0, total = 0; uint32_t slices = 0;
    uint32_t slice = is3d ? (uint32_t)o.z : layer;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &loff, &rp, &sp, &slices, &total) || slice >= slices) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 解码贴图回读：切片越界"); return;
    }
    if (!bc_shadow_ensure(c, t, total)) return;   // 一次都没上传过：影子建出来是全零（原机上是未定义内容）
    VkBufferCopy *rg = calloc((size_t)bh, sizeof *rg);
    if (!rg) { set_sticky(c, NVN_GPU_E_OOM, "BC 解码贴图回读：拷贝表分配失败"); return; }
    uint64_t src = loff + (uint64_t)slice * sp + (uint64_t)(o.y / 4) * rp + (uint64_t)(o.x / 4) * (uint32_t)bb;
    for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){src + y * rp, off + y * row_bytes, packed_row};
    full_barrier(c);
    vkCmdCopyBuffer(c->cb, t->bc_sbuf, b->buffer, (uint32_t)bh, rg);
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    free(rg);
    cmd_ref(c, b, REF_BUF);
    cmd_ref(c, t, REF_TEX);
}

// 第一次上传时建压缩影子缓冲（大小按 bc_shadow_layout 的总长）。失败置粘滞。
static int bc_shadow_ensure(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint64_t total) {
    if (t->bc_sbuf) return 1;
    VkBufferCreateInfo bi = {0}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = total ? total : 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkMemoryRequirements req;
    if (vkCreateBuffer(g.d, &bi, NULL, &buf) != VK_SUCCESS) { set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子缓冲建不出（%llu 字节）", (unsigned long long)total); return 0; }
    vkGetBufferMemoryRequirements(g.d, buf, &req);
    uint32_t type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (type == UINT32_MAX) type = s2a_vk_memory_type(g.dev, req.memoryTypeBits, 0, NULL);
    VkMemoryAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(g.d, &ai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(g.d, buf, mem, 0) != VK_SUCCESS) {
        if (mem) vkFreeMemory(g.d, mem, NULL);
        vkDestroyBuffer(g.d, buf, NULL);
        set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子显存分配失败（%llu 字节）", (unsigned long long)total); return 0;
    }
    t->bc_sbuf = buf; t->bc_smem = mem;
    full_barrier(c);
    vkCmdFillBuffer(c->cb, buf, 0, VK_WHOLE_SIZE, 0);   // 没上传过的区域回读为零，不留上一块显存的旧内容
    c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
    return 1;
}

// BC 贴图写入区域与源排布的校验（bc_upload 与缓冲 → BC 贴图共用）。返回 1 = 合法，并给出区域、块尺寸、源切片行距。
static int bc_src_check(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        uint64_t row_bytes, uint64_t image_bytes, VkOffset3D *offp, VkExtent3D *extp, int *bbp,
                        uint64_t *bwp, uint64_t *bhp, uint64_t *slice_stridep) {
    VkOffset3D off; VkExtent3D ext; int bb = 0;
    // 原先四种原因合成一句「子资源或区域非法」，真机读数分不出是哪一种（#697 Mali）：逐条报，带上数值
    if (level >= t->levels || layer >= t->layers) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传子资源越界：级 %u / 共 %u，层 %u / 共 %u", level, t->levels, layer, t->layers); return 0;
    }
    if (!nvn_vk_bcdec_route(t->nvn_format, NULL, NULL, NULL, NULL, &bb)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传：格式 %#x 没有现解路线", t->nvn_format); return 0;
    }
    if (copy_region(t, level, r, &off, &ext) || !ext.width || !ext.height || !ext.depth) {
        uint32_t mw = t->w >> level, mh = t->h >> level, md = t->depth >> level;
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传区域超出这一级：级 %u（%ux%ux%u），区域 (%d,%d,%d) %dx%dx%d，目标 %u",
                   level, mw ? mw : 1, mh ? mh : 1, md ? md : 1, r ? r->xoffset : 0, r ? r->yoffset : 0, r ? r->zoffset : 0,
                   r ? r->width : 0, r ? r->height : 0, r ? r->depth : 0, t->target);
        return 0;
    }
    uint32_t mw = t->w >> level, mh = t->h >> level; if (!mw) mw = 1; if (!mh) mh = 1;
    if ((off.x & 3) || (off.y & 3) || ((ext.width & 3) && (uint32_t)off.x + ext.width != mw) ||
        ((ext.height & 3) && (uint32_t)off.y + ext.height != mh)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传区域未按压缩块对齐"); return 0;
    }
    uint64_t bw = ((uint64_t)ext.width + 3) / 4, bh = ((uint64_t)ext.height + 3) / 4;
    uint64_t packed_row = bw * (uint32_t)bb;
    if (row_bytes < packed_row || bh > SIZE_MAX / packed_row ||
        (bh > 1 && row_bytes > (UINT64_MAX - packed_row) / (bh - 1)) || image_bytes < (bh - 1) * row_bytes + packed_row) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源行距或长度非法"); return 0;
    }
    // image_bytes 是整个体积的可读长度；相邻 z 片相隔 bh 个源块行，不是 image_bytes。
    if (row_bytes > SIZE_MAX / bh) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源切片行距溢出"); return 0;
    }
    uint64_t slice_stride = row_bytes * bh;
    uint64_t last_slice = (bh - 1) * row_bytes + packed_row;
    if ((uint64_t)(ext.depth - 1) > (SIZE_MAX - last_slice) / slice_stride ||
        image_bytes < (uint64_t)(ext.depth - 1) * slice_stride + last_slice) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传源体积长度非法"); return 0;
    }
    *offp = off; *extp = ext; *bbp = bb; *bwp = bw; *bhp = bh; *slice_stridep = slice_stride;
    return 1;
}

// 解码用的临时图：解出来的像素先落在这里（带 STORAGE 的可变格式），再按区域拷进目标贴图（bc_upload 与缓冲 → BC 贴图共用）。
static struct bc_temp *bc_temp_image(const struct nvn_gpu_tex_s *t, VkExtent3D ext) {
    struct bc_temp *tmp = calloc(1, sizeof *tmp);
    if (!tmp) return NULL;
    VkImageCreateInfo ci = {0}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = t->format; ci.extent = (VkExtent3D){ext.width, ext.height, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements mr; VkMemoryAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    if (vkCreateImage(g.d, &ci, NULL, &tmp->image) != VK_SUCCESS) goto fail;
    vkGetImageMemoryRequirements(g.d, tmp->image, &mr);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = s2a_vk_memory_type(g.dev, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.d, &ai, NULL, &tmp->memory) != VK_SUCCESS ||
        vkBindImageMemory(g.d, tmp->image, tmp->memory, 0) != VK_SUCCESS) goto fail;
    return tmp;
fail:
    if (tmp->image) vkDestroyImage(g.d, tmp->image, NULL);
    if (tmp->memory) vkFreeMemory(g.d, tmp->memory, NULL);
    free(tmp);
    return NULL;
}

// 解码好的临时图按区域拷进目标贴图（z 片 / 层由调用方给）。
static void bc_temp_to_tex(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, struct bc_temp *tmp, uint32_t level, uint32_t layer,
                           VkOffset3D off, VkExtent3D ext, uint32_t z) {
    VkImageMemoryBarrier b = {0}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = tmp->image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cp = {0}; cp.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; cp.dstOffset = off; cp.dstOffset.z += (int32_t)z;
    cp.extent = (VkExtent3D){ext.width, ext.height, 1};
    vkCmdCopyImage(c->cb, tmp->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL); cmd_ref(c, t, REF_TEX);
}

static void bc_upload(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer,
                      const NVNcopyRegion *r, const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    VkOffset3D off; VkExtent3D ext; int bb = 0; uint64_t bw = 0, bh = 0, slice_stride = 0;
    if (!bc_src_check(c, t, level, layer, r, row_bytes, image_bytes, &off, &ext, &bb, &bw, &bh, &slice_stride)) return;
    uint64_t packed_row = bw * (uint32_t)bb;
    uint64_t sh_loff = 0, sh_rp = 0, sh_sp = 0, sh_total = 0; uint32_t sh_slices = 0;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &sh_loff, &sh_rp, &sh_sp, &sh_slices, &sh_total)) {
        set_sticky(c, NVN_GPU_E_ARG, "BC 上传：压缩影子布局溢出"); return;
    }
    if (!bc_shadow_ensure(c, t, sh_total)) return;
    size_t bytes = (size_t)(packed_row * bh); uint8_t *packed = malloc(bytes);
    struct bc_temp *tmp = NULL;
    if (!packed) { set_sticky(c, NVN_GPU_E_OOM, "BC 上传暂存分配失败"); return; }
    for (uint32_t z = 0; z < ext.depth; z++) {
        for (uint64_t y = 0; y < bh; y++) memcpy(packed + y * packed_row, (const uint8_t *)src + (uint64_t)z * slice_stride + y * row_bytes, (size_t)packed_row);
        {   // 同一份原字节按序录进压缩影子缓冲（回读用，见 bc_readback）
            uint32_t s = t->target == NVN_TEXTURE_TARGET_3D ? (uint32_t)off.z + z : layer;
            nvn_gpu_slice ss; nvn_gpu_err se; VkBufferCopy *rg;
            if (s >= sh_slices) { set_sticky(c, NVN_GPU_E_ARG, "BC 上传：切片越过压缩影子"); free(packed); return; }
            if (!rc_is(nvn_gpu_arena_alloc(c, bytes, 4, &ss, &se), NVN_GPU_OK) || !(rg = calloc((size_t)bh, sizeof *rg))) {
                if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "BC 上传：压缩影子暂存分配失败");
                free(packed); return;
            }
            memcpy(ss.cpu, packed, bytes);
            uint64_t dst = sh_loff + (uint64_t)s * sh_sp + (uint64_t)(off.y / 4) * sh_rp + (uint64_t)(off.x / 4) * (uint32_t)bb;
            for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){ss.off + y * packed_row, dst + y * sh_rp, packed_row};
            full_barrier(c);
            vkCmdCopyBuffer(c->cb, ss.buf->buffer, t->bc_sbuf, (uint32_t)bh, rg);
            c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
            free(rg);
        }
        tmp = bc_temp_image(t, ext);
        if (!tmp) goto fail;
        char why[192] = {0}; int result = -1;
        pthread_mutex_lock(&g_submit_mu);
        if (g.bc || nvn_vk_bcdec_create(g.dev, &g.bc, why, sizeof why) == 0)
            result = nvn_vk_bcdec_decode(g.bc, g.dev, tmp->image, t->nvn_format, ext.width, ext.height, 0, 0, packed, bytes, why, sizeof why);
        if (result == -2) { tmp->next = g.bc_pending; g.bc_pending = tmp; }
        pthread_mutex_unlock(&g_submit_mu);
        if (result != 0) {
            set_sticky(c, NVN_GPU_E_DEVICE, "BC 解码失败：%s", why);
            if (result == -2) { free(packed); return; } // 临时图转交会话，close 取得完成证明后释放
            vkDestroyImage(g.d, tmp->image, NULL); vkFreeMemory(g.d, tmp->memory, NULL); free(tmp);
            free(packed); return;   // 粘滞原因已是解码失败，不再改写
        }
        tmp->next = c->bc_temps; c->bc_temps = tmp;
        bc_temp_to_tex(c, t, tmp, level, layer, off, ext, z);
    }
    free(packed);
    return;
fail:
    free(packed);
    set_sticky(c, NVN_GPU_E_OOM, "BC 临时图或解码初始化失败");
}

// 段内解码的命令（资源由 nvn_vk_bcdec_prepare 建好）：在这里用后端的 vkCmd* 录——两级回放开着时这些名字走包装进录制线程，
// 和段里前后的拷贝保持同一顺序（模块里直接调 vkCmd* 会越过录制线程、跑到前面那几条拷贝之前）。
static void bc_record_decode(struct nvn_gpu_cmd_s *c, const struct nvn_vk_bcdec_rec *rec, VkImage dst,
                             VkBuffer src, VkDeviceSize src_off, uint32_t w, uint32_t h) {
    VkImageMemoryBarrier b = {0}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = rec->in; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {0};
    cp.bufferOffset = src_off;
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){rec->bw, rec->bh, 1};
    vkCmdCopyBufferToImage(c->cb, src, rec->in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageMemoryBarrier db = {0}; db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;   // 临时图整张被覆写：UNDEFINED → GENERAL
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = dst; db.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    db.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &db);
    vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, rec->pipe);
    vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, rec->pl, 0, 1, &rec->ds, 0, NULL);
    int32_t res[2] = { (int32_t)w, (int32_t)h };
    vkCmdPushConstants(c->cb, rec->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, res);
    vkCmdDispatch(c->cb, (w + 7) / 8, (h + 7) / 8, 1);
    // 结果交给 bc_temp_to_tex 的屏障（COMPUTE 写 → TRANSFER 读，同时转 TRANSFER_SRC）
}

// 缓冲 → BC 现解贴图（#697）：游戏把压缩块从 GPU 缓冲拷进 BC 贴图（常见是同一段里刚从另一张 BC 贴图回读出来的字节），
// 录制时 CPU 上没有这些字节，bc_upload 那条「CPU 字节一次性提交解码」用不上。整条在段里按 GPU 顺序做：
//   ① 源行（带行距）拷成段内暂存区里的紧凑块（16 字节对齐）；② 同一份块拷进压缩影子（回读用，同 bc_upload）；
//   ③ 解码录进本段（nvn_vk_bcdec_prepare 建资源、bc_record_decode 录命令）落到临时图；④ 临时图按区域拷进目标。临时资源随段完成回收（bc_temps）。
// 解码绑了自己的计算管线和描述符组：录完让后端的描述符布局缓存失效，下一次派发重绑。
static void bc_copy_from_buf(struct nvn_gpu_cmd_s *c, struct nvn_gpu_tex_s *t, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                             struct nvn_gpu_buf_s *b, uint64_t boff, uint64_t row_bytes, uint64_t image_bytes) {
    VkOffset3D off; VkExtent3D ext; int bb = 0; uint64_t bw = 0, bh = 0, slice_stride = 0;
    if (!bc_src_check(c, t, level, layer, r, row_bytes, image_bytes, &off, &ext, &bb, &bw, &bh, &slice_stride)) return;
    if (boff > b->bytes || image_bytes > b->bytes - boff) { set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：源越过缓冲末尾"); return; }
    uint64_t packed_row = bw * (uint32_t)bb, bytes = packed_row * bh;
    uint64_t sh_loff = 0, sh_rp = 0, sh_sp = 0, sh_total = 0; uint32_t sh_slices = 0;
    if (!bc_shadow_layout(t, (uint32_t)bb, level, &sh_loff, &sh_rp, &sh_sp, &sh_slices, &sh_total)) {
        set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：压缩影子布局溢出"); return;
    }
    if (!bc_shadow_ensure(c, t, sh_total)) return;
    char why[192] = {0};
    pthread_mutex_lock(&g_submit_mu);
    int ready = g.bc || nvn_vk_bcdec_create(g.dev, &g.bc, why, sizeof why) == 0;
    pthread_mutex_unlock(&g_submit_mu);
    if (!ready) { set_sticky(c, NVN_GPU_E_DEVICE, "BC 解码初始化失败：%s", why); return; }
    VkBufferCopy *rg = calloc((size_t)bh, sizeof *rg);
    if (!rg) { set_sticky(c, NVN_GPU_E_OOM, "缓冲 → BC 贴图：拷贝区域表分配失败"); return; }
    cmd_ref(c, b, REF_BUF);
    for (uint32_t z = 0; z < ext.depth && rc_is(c->sticky, NVN_GPU_OK); z++) {
        uint32_t s = t->target == NVN_TEXTURE_TARGET_3D ? (uint32_t)off.z + z : layer;
        if (s >= sh_slices) { set_sticky(c, NVN_GPU_E_ARG, "缓冲 → BC 贴图：切片越过压缩影子"); break; }
        nvn_gpu_slice ss; nvn_gpu_err se;
        if (!rc_is(nvn_gpu_arena_alloc(c, bytes, 16, &ss, &se), NVN_GPU_OK)) {
            if (rc_is(c->sticky, NVN_GPU_OK)) set_sticky(c, NVN_GPU_E_OOM, "缓冲 → BC 贴图：暂存分配失败");
            break;
        }
        full_barrier(c);   // 源缓冲此前在本段里的写（如回读）对拷贝可见
        for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){boff + (uint64_t)z * slice_stride + y * row_bytes, ss.off + y * packed_row, packed_row};
        vkCmdCopyBuffer(c->cb, b->buffer, ss.buf->buffer, (uint32_t)bh, rg);
        c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
        full_barrier(c);
        uint64_t dst = sh_loff + (uint64_t)s * sh_sp + (uint64_t)(off.y / 4) * sh_rp + (uint64_t)(off.x / 4) * (uint32_t)bb;
        for (uint64_t y = 0; y < bh; y++) rg[y] = (VkBufferCopy){ss.off + y * packed_row, dst + y * sh_rp, packed_row};
        vkCmdCopyBuffer(c->cb, ss.buf->buffer, t->bc_sbuf, (uint32_t)bh, rg);
        c->xfer_dirty = 1;   // 缓冲作目标的传输：下一次计算派发前要补全屏障（见 nvn_gpu_dispatch）
        struct bc_temp *tmp = bc_temp_image(t, ext);
        if (!tmp) { set_sticky(c, NVN_GPU_E_OOM, "缓冲 → BC 贴图：临时图分配失败"); break; }
        if (nvn_vk_bcdec_prepare(g.bc, g.dev, tmp->image, t->nvn_format, ext.width, ext.height, 0, 0, &tmp->rec, why, sizeof why) != 0) {
            vkDestroyImage(g.d, tmp->image, NULL); vkFreeMemory(g.d, tmp->memory, NULL); free(tmp);
            set_sticky(c, NVN_GPU_E_DEVICE, "BC 解码失败：%s", why); break;
        }
        bc_record_decode(c, &tmp->rec, tmp->image, ss.buf->buffer, ss.off, ext.width, ext.height);
        c->desc_layout = VK_NULL_HANDLE;   // 计算绑定点被解码占过：下一次派发重绑描述符组
        tmp->next = c->bc_temps; c->bc_temps = tmp;
        bc_temp_to_tex(c, t, tmp, level, layer, off, ext, z);
    }
    free(rg);
}

void nvn_gpu_tex_upload(nvn_gpu_cmd ch, nvn_gpu_tex th, uint32_t level, uint32_t layer, const NVNcopyRegion *r,
                        const void *src, uint64_t row_bytes, uint64_t image_bytes) {
    { struct nvn_gpu_cmd_s *cc = ch; if (cc && cc->in_pass && rc_is(cc->sticky, NVN_GPU_OK)) { set_sticky(cc, NVN_GPU_E_ORDER, "传输操作在通道内"); return; } }
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!src) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload src 为空"); return; }
    struct nvn_gpu_tex_s *t = th;
    if (!t) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 贴图为空"); return; }
    if (t->bc_decoded) { bc_upload(c, t, level, layer, r, src, row_bytes, image_bytes); return; }
    int ebpp = 0;
    int expand = nvn_fmt_expand_target(t->nvn_format, NULL, &ebpp);
    uint32_t ew = r ? (uint32_t)r->width : (t->w >> level);
    uint32_t eh = r ? (uint32_t)r->height : (t->h >> level);
    if (!ew) ew = 1;
    if (!eh) eh = 1;
    if (expand && (r && (r->width <= 0 || r->height <= 0))) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开区域非法"); return; }
    uint64_t out_row = expand ? (uint64_t)ebpp * ew : 0;
    uint64_t out_bytes = expand ? out_row * eh : 0;
    if (expand) {
        nvn_fmt_info in;
        if (!nvn_fmt_info_of(t->nvn_format, &in)) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload 源格式未知"); return; }
        uint64_t need_row = (uint64_t)in.bytes * ew;
        uint64_t need_img = (uint64_t)row_bytes * (eh - 1) + need_row;
        if (row_bytes < need_row || image_bytes < need_img) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开源越界（行 %llu < %llu 或总 %llu < %llu）",
                       (unsigned long long)row_bytes, (unsigned long long)need_row,
                       (unsigned long long)image_bytes, (unsigned long long)need_img);
            return;
        }
    }
    uint64_t need = expand ? out_bytes : image_bytes;
    nvn_gpu_slice sl;
    nvn_gpu_err e;
    // 暂存偏移要是目标格式一个纹素块字节数的倍数（Vulkan 对缓冲到图像拷贝的要求；压缩格式一块 8 / 16 字节，按 4 对齐会落在块中间）
    uint32_t align = 4;
    { nvn_fmt_info bi; uint32_t bb = expand ? (uint32_t)ebpp : nvn_fmt_info_of(t->nvn_format, &bi) ? bi.bytes : 0;
      if (bb > 4 && !(bb & (bb - 1))) align = bb; }
    if (!rc_is(nvn_gpu_arena_alloc(ch, need, align, &sl, &e), NVN_GPU_OK)) {
        set_sticky(c, e.rc.code, "tex_upload 暂存：%s", e.why);
        return;
    }
    if (expand) {
        if (nvn_fmt_expand_upload(t->nvn_format, src, (size_t)row_bytes, ew, eh,
                                  sl.cpu, (size_t)out_row) != 0) {
            set_sticky(c, NVN_GPU_E_ARG, "tex_upload 展开失败（格式 0x%x）", t->nvn_format);
            return;
        }
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, out_row, out_bytes, th, level, layer, r);
    } else if (nvn_vk_format_substituted(t->nvn_format)) {
        // #398：D24 换成 D32 存储。按原 D24 深度面口径（每纹素 4 字节，低 24 位 UNORM）换成 float 再传深度面。
        if (row_bytes & 3) { set_sticky(c, NVN_GPU_E_ARG, "tex_upload D24 行宽未 4 对齐"); return; }
        memcpy(sl.cpu, src, (size_t)image_bytes);
        nvn_vk_depth24_to_f32(sl.cpu, (size_t)image_bytes / 4);
        copy_buf_tex_impl(ch, sl.buf, sl.off, row_bytes, image_bytes, th, level, layer, r, 1);
    } else {
        memcpy(sl.cpu, src, (size_t)image_bytes);
        nvn_gpu_copy_buf_tex(ch, sl.buf, sl.off, row_bytes, image_bytes, th, level, layer, r);
    }
}

#include "backend_vulkan_clear.inc"

void nvn_gpu_clear_rect(nvn_gpu_cmd ch, const nvn_gpu_clear *d) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "clear_rect 在通道外"); return; }
    es_reset(c);   // 内部画一笔整屏三角形：换管线（只声明了部分动态状态）、改视口裁剪与模板、写 push；下一笔绘制全部重发（#708）
    if (!d || (d->color < 0 && !d->depth && !d->stencil)) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 什么都不清"); return; }
    if (d->color >= 0 && ((uint32_t)d->color >= c->pass_ncolor || !(c->pass_color_mask >> d->color & 1))) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 颜色槽 %d 越界或是空位（通道有 %u 个槽）", d->color, c->pass_ncolor); return; }
    if ((d->depth && !(c->pass_ds_aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) || (d->stencil && !(c->pass_ds_aspect & VK_IMAGE_ASPECT_STENCIL_BIT))) {
        set_sticky(c, NVN_GPU_E_ARG, "clear_rect 要清的深度 / 模板面不在通道里"); return;
    }
    if (!d->w || !d->h || d->x < 0 || d->y < 0 || (uint64_t)d->x + d->w > c->pass_w || (uint64_t)d->y + d->h > c->pass_h) {
        set_sticky(c, NVN_GPU_E_ARG, "clear_rect 矩形 %d,%d %ux%u 为空或超出通道 %ux%u", d->x, d->y, d->w, d->h, c->pass_w, c->pass_h); return;
    }
    uint32_t cm = d->color >= 0 ? d->color_mask & 0xfu : 0xfu, sm = d->stencil ? d->stencil_mask : 0xffu;
    if (!cm || !sm) { set_sticky(c, NVN_GPU_E_ARG, "clear_rect 写掩码为 0（颜色 %#x 模板 %#x）", (unsigned)d->color_mask, (unsigned)d->stencil_mask); return; }
    // 一律画一笔整屏三角形（backend_vulkan_clear.inc：不用附件清除命令，理由见那里）。颜色、深度、模板同一笔做
    clr_draw(c, d->color, cm, &d->value, d->depth != 0, d->stencil ? sm : 0, d->stencil_value, d);
    c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;   // 内部清除改绑状态：下一笔全部重建
}

void nvn_gpu_barrier(nvn_gpu_cmd ch, uint32_t nvn_bits) {
    (void)nvn_bits;
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    // 保守实现：先把所有已写内存对后续所有阶段可见；按 NVN 位收窄留给后续。
    full_barrier(c);
}

// 带缩放 / 换格式的贴图拷贝（vkCmdBlitImage）。源、目标都取视图的第一级第一层；区域按各自那一级的像素给。
// 只做单采样、两边都不是整数 / 深度模板格式、视图没换格式的情形；格式对不支持（设备的格式特性里没有 BLIT_SRC / BLIT_DST，
// 或要线性过滤而源不支持）报 UNSUPPORTED。目标是 sRGB 格式时写进去的值按 sRGB 编码（同把它当颜色附件画进去）。
void nvn_gpu_blit(nvn_gpu_cmd ch, nvn_gpu_view sv, const NVNcopyRegion *sr, nvn_gpu_view dv, const NVNcopyRegion *dr, int linear) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (c->in_pass) { set_sticky(c, NVN_GPU_E_ORDER, "传输操作在通道内"); return; }
    if (!sv || !dv || !sv->tex || !dv->tex || !sr || !dr) { set_sticky(c, NVN_GPU_E_ARG, "blit 参数为空"); return; }
    struct nvn_gpu_tex_s *s = sv->tex, *d = dv->tex;
    if (sv->format != s->format || dv->format != d->format) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：视图换了格式（拷贝按存储的格式做）"); return; }
    if (s->samples > 1 && d->samples == 1) {
        nvn_fmt_info f;
        if (linear || s->w != d->w || s->h != d->h || s->format != d->format || s->nvn_format != d->nvn_format ||
            s->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
            !nvn_fmt_info_of(s->nvn_format, &f) || f.kind == NVN_FMT_UINT || f.kind == NVN_FMT_SINT || f.depth || f.stencil ||
            s->target == NVN_TEXTURE_TARGET_3D || d->target == NVN_TEXTURE_TARGET_3D) {
            // resolve 直接用 VkImage + 子资源范围画，不用采样视图的通道映射：视图带映射（游戏设的 / 没有 alpha 的格式把 a 钉成 ONE）不影响
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "resolve：只支持同格式非整数颜色二维贴图"); return;
        }
        VkOffset3D so, dof; VkExtent3D se, de;
        if (sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0 ||
            sr->depth != 1 || dr->depth != 1 || sr->zoffset || dr->zoffset || copy_region(s, sv->base_level, sr, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) ||
            sv->base_layer >= s->layers || dv->base_layer >= d->layers ||
            se.width != de.width || se.height != de.height || se.depth != 1 || de.depth != 1 || so.z || dof.z) {
            set_sticky(c, NVN_GPU_E_ARG, "resolve：区域越界或尺寸不相同"); return;
        }
        full_barrier(c);
        image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve ir = {0};
        ir.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, sv->base_level, sv->base_layer, 1};
        ir.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, dv->base_level, dv->base_layer, 1};
        ir.srcOffset = so; ir.dstOffset = dof; ir.extent = se;
        vkCmdResolveImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ir);
        image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL); image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
        cmd_ref(c, s, REF_TEX); cmd_ref(c, d, REF_TEX);
        return;
    }
    if (s->samples > 1 || d->samples > 1 || nvn_vk_format_is_depth(s->format) || nvn_vk_format_is_depth(d->format)) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：多重采样 / 深度模板没接"); return; }
    { nvn_fmt_info a, b; int ai = nvn_fmt_info_of(s->nvn_format, &a) && (a.kind == NVN_FMT_UINT || a.kind == NVN_FMT_SINT), bi = nvn_fmt_info_of(d->nvn_format, &b) && (b.kind == NVN_FMT_UINT || b.kind == NVN_FMT_SINT);
      if (ai || bi) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：整数格式没接"); return; } }
    VkFormatProperties fs, fd; vkGetPhysicalDeviceFormatProperties(g.phys, s->format, &fs); vkGetPhysicalDeviceFormatProperties(g.phys, d->format, &fd);
    if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        (linear && !(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))) { set_sticky(c, NVN_GPU_E_UNSUPPORTED, "blit：设备不支持这对格式（%d → %d，线性 %d）", (int)s->format, (int)d->format, linear); return; }
    VkOffset3D so, dof; VkExtent3D se, de;
    if (copy_region(s, sv->base_level, sr, &so, &se) || copy_region(d, dv->base_level, dr, &dof, &de) || sv->base_layer >= s->layers || dv->base_layer >= d->layers) { set_sticky(c, NVN_GPU_E_ARG, "blit 区域越界"); return; }
    full_barrier(c);
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit ib; memset(&ib, 0, sizeof ib);
    ib.srcSubresource = (VkImageSubresourceLayers){s->aspect, sv->base_level, sv->base_layer, 1};
    ib.dstSubresource = (VkImageSubresourceLayers){d->aspect, dv->base_level, dv->base_layer, 1};
    ib.srcOffsets[0] = so; ib.srcOffsets[1] = (VkOffset3D){so.x + (int32_t)se.width, so.y + (int32_t)se.height, 1};
    ib.dstOffsets[0] = dof; ib.dstOffsets[1] = (VkOffset3D){dof.x + (int32_t)de.width, dof.y + (int32_t)de.height, 1};
    vkCmdBlitImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, d, VK_IMAGE_LAYOUT_GENERAL);
    cmd_ref(c, s, REF_TEX);
    cmd_ref(c, d, REF_TEX);
}

// 之后的绘制计入 slot，-1 停止。只在声明了 occlusion 的通道里；一个槽一段里只能开一次（Vulkan 的查询重置后只能用一回）。
// 设备有精确计数就要精确值，没有就只保证「有样本通过时非 0」。
void nvn_gpu_occlusion(nvn_gpu_cmd ch, int32_t slot) {
    struct nvn_gpu_cmd_s *c = ch;
    if (!c || !rc_is(c->sticky, NVN_GPU_OK)) return;
    if (!c->in_pass || !c->pass_occl) { set_sticky(c, NVN_GPU_E_ORDER, "遮挡查询只能在声明了 occlusion 的通道里开关"); return; }
    if (!c->qpool) { set_sticky(c, NVN_GPU_E_ARG, "遮挡查询：本段开段时没声明 occlusion_slots"); return; }
    if (slot >= 0 && ((uint32_t)slot >= c->qcount || c->q_used[slot])) { set_sticky(c, NVN_GPU_E_ARG, "遮挡查询槽 %d 越界（本段 %u 槽）或本段已用过", slot, c->qcount); return; }
    if (c->q_active >= 0) { vkCmdEndQuery(c->cb, c->qpool, (uint32_t)c->q_active); c->q_active = -1; }
    if (slot < 0) return;
    // 开关 debug.s2a.nvk_occl_fast（缺省 0 = 照能力位用 PRECISE 精确计数）：1 = 不带 PRECISE，走显卡的快速（可能非精确）计数。
    // 精确计数在部分移动 GPU 上要逐个样本过一遍，本款每帧 400 多个遮挡查询、全在热通道里；NVN 的语义要精确值，先用读数看差多少。
    static int fast = -1;
    if (fast < 0) fast = nvn_plat_flag("nvk_occl_fast", 0) != 0;
    OC(OC_QUERY);
    vkCmdBeginQuery(c->cb, c->qpool, (uint32_t)slot, g.dev->core_features.occlusionQueryPrecise && !fast ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    c->q_active = slot; c->q_used[slot] = 1; if ((uint32_t)slot + 1 > c->qmax_used) c->qmax_used = (uint32_t)slot + 1;
}

void nvn_gpu_label(nvn_gpu_cmd c, const char *text) { (void)c; (void)text; }

void nvn_gpu_tex_upload_blocklinear(nvn_gpu_cmd c, nvn_gpu_tex t, uint32_t l, uint32_t y, const nvn_texlayout *L, const void *src, uint64_t bytes) {
    (void)t; (void)l; (void)y; (void)L; (void)src; (void)bytes; VK_NOT_YET(c, "nvn_gpu_tex_upload_blocklinear");
}
// 上屏 6 个入口在 backend_vulkan_wsi.inc（文件末尾 include：它要用后面定义的提交与回收函数）

// ---------------------------------------------------------------- 命令

nvn_gpu_rc nvn_gpu_cmd_begin(const nvn_gpu_cmd_desc *d, nvn_gpu_cmd *out, nvn_gpu_err *err) {
    if (!out) return fail(err, NVN_GPU_E_ARG, "out 为空");
    *out = NULL;
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许开命令段");
    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, failed = g.failed != 0;
    pthread_mutex_unlock(&g_mu);
    if (closing) return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    if (failed) return fail(err, NVN_GPU_E_FAILED, "时间线已进入失败终态（恢复只能关会话重开）");
    if (lost) return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");

    struct nvn_gpu_cmd_s *c = calloc(1, sizeof *c);
    if (!c) return fail(err, NVN_GPU_E_OOM, "cmd 分配失败");
    memset(c->desc_sets, 0, sizeof c->desc_sets); c->desc_layout = VK_NULL_HANDLE; c->desc_dirty = 0xfu;
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = g.dev->queue_family;
    if (!seg_reuse_on() || !seg_take_cmd(&c->pool, &c->cb)) {   // 空闲表里没有：新建一个池和它的命令缓冲
    OC(OC_CPOOL);
    if (vkCreateCommandPool(g.d, &pci, NULL, &c->pool) != VK_SUCCESS) {
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkCreateCommandPool 失败");
    }
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.d, &ai, &c->cb) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_OOM, "vkAllocateCommandBuffers 失败");
    }
    }
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->cb, &bi) != VK_SUCCESS) {
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_DEVICE, "vkBeginCommandBuffer 失败");
    }
    c->recording = 1;
    c->xfer_dirty = 1;   // 前置段 / 上一段的传输写：段末屏障只对主机可见，本段第一次派发前补一道
    c->sticky = rc(NVN_GPU_OK);
    c->q_active = -1;
    c->opt = nvn_plat_flag("nvk_opt_layout", 0) != 0;   // 附件 / 采样用对齐语义的布局（缺省 0 = 全 GENERAL 的旧行为）
    {   // 调试读数（debug 开关 nvk_gputime）：段首尾各写一个 GPU 时间戳，完成时算这一段在 GPU 上从开始到结束用了多久
        static int on = -1; if (on < 0) on = nvn_plat_flag("nvk_gputime", 0);
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 + 2 * TS_PASS_MAX;
        if (on && vkCreateQueryPool(g.d, &qi, NULL, &c->tsq) == VK_SUCCESS) {
            vkCmdResetQueryPool(c->cb, c->tsq, 0, 2 + 2 * TS_PASS_MAX);
            vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->tsq, 0);
        } else c->tsq = VK_NULL_HANDLE;
    }
    if (d && d->occlusion_slots) {   // 查询池：整池重置只能在通道外，放在段的最开头
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = d->occlusion_slots;
        c->q_used = calloc(d->occlusion_slots, 1);
        if (c->q_used && seg_reuse_on()) c->qpool = seg_take_q(d->occlusion_slots);
        if (!c->q_used || (!c->qpool && vkCreateQueryPool(g.d, &qi, NULL, &c->qpool) != VK_SUCCESS)) {
            free(c->q_used); vkDestroyCommandPool(g.d, c->pool, NULL); free(c);
            return fail(err, NVN_GPU_E_OOM, "遮挡查询池（%u 槽）创建失败", d->occlusion_slots);
        }
        c->qcount = d->occlusion_slots;
        vkCmdResetQueryPool(c->cb, c->qpool, 0, c->qcount);
    }
    pthread_mutex_lock(&g_mu);
    if (g.closing) {
        pthread_mutex_unlock(&g_mu);
        if (c->tsq) vkDestroyQueryPool(g.d, c->tsq, NULL);
        if (c->qpool) vkDestroyQueryPool(g.d, c->qpool, NULL);
        free(c->q_used);
        vkDestroyCommandPool(g.d, c->pool, NULL);
        free(c);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    c->live_next = g.live;
    g.live = c;
    pthread_mutex_unlock(&g_mu);
    *out = c;
    return rc(NVN_GPU_OK);
}

void nvn_gpu_cmd_abort(nvn_gpu_cmd ch) {
    if (!ch || !g_open) return;
    struct nvn_gpu_cmd_s *c = ch;
    int owned = 0;
    pthread_mutex_lock(&g_mu);
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (*pp) {
        *pp = c->live_next;
        c->live_next = NULL;
        owned = 1;   // 摘链与销毁所有权原子完成；并发 abort / close 不会重复释放
    }
    pthread_mutex_unlock(&g_mu);
    if (owned) finish_cmd(c, 0);   // 不执行、不回调、不推进时间线；引用随即解除
}

static void enqueue_segment(struct nvn_gpu_vk *vk, struct vk_segment *s) {
    pthread_mutex_lock(&g_mu);
    if (vk->qtail) vk->qtail->next = s;
    else vk->qhead = s;
    vk->qtail = s;
    if (!s->internal && !submitted_add(vk, s->signal) && (!vk->failed || s->signal < vk->failed)) vk->failed = s->signal;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void publish_signal(uint64_t signal, int failed) {
    pthread_mutex_lock(&g_mu);
    g.last_signal = signal;
    if (failed) {
        if (!g.failed || signal < g.failed) g.failed = signal;
    } else {
        g.stats[STAT_SUBMITS]++;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// 持 g_submit_mu，c 已录完。待转表不空：在 c 的池里另录一个命令缓冲，把表里的图像从 UNDEFINED 转到 GENERAL（见「图像布局」），
// 图像各加一份引用记在 c 上（段完成归还；段没执行由 finish_cmd 放回表）。cbs 填本次提交要带的命令缓冲（转布局的在前），
// 回个数；0 = 分配 / 录制失败。
static uint32_t submit_cbs(struct nvn_gpu_cmd_s *c, VkCommandBuffer cbs[2]) {
    pthread_mutex_lock(&g_mu);
    uint32_t n = g_newimg_n;
    struct nvn_gpu_tex_s **v = NULL;
    if (n) {
        v = malloc((size_t)n * sizeof *v);
        if (!v) { pthread_mutex_unlock(&g_mu); return 0; }
        memcpy(v, g_newimg, (size_t)n * sizeof *v);
        for (uint32_t i = 0; i < n; i++) v[i]->refs++;
        g_newimg_n = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (!n) { cbs[0] = c->cb; return 1; }
    c->newimg = v; c->n_newimg = n;
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = c->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.d, &ai, &c->init_cb) != VK_SUCCESS) { c->init_cb = VK_NULL_HANDLE; return 0; }
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->init_cb, &bi) != VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < n; i++) {
        VkImageMemoryBarrier b;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = v[i]->image;
        b.subresourceRange = (VkImageSubresourceRange){v[i]->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(c->init_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    if (vkEndCommandBuffer(c->init_cb) != VK_SUCCESS) return 0;
    cbs[0] = c->init_cb; cbs[1] = c->cb;
    return 2;
}

// 提交串行化：signal 校验、vkEndCommandBuffer、vkQueueSubmit、入队必须在同一临界区，
// 否则两个提交线程可能让更大的 signal 先入队，或同时通过 last_signal 检查。
static nvn_gpu_rc commit_impl(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    struct nvn_gpu_cmd_s *c = ch;

    struct vk_segment *s = calloc(1, sizeof *s);
    if (!s) {
        finish_cmd(c, 0);
        return fail(err, NVN_GPU_E_OOM, "段记录分配失败（本段未提交）");
    }
    s->cmd = c;
    s->signal = signal;
    s->done = done;
    s->user = user;
    s->cb = c->cb;

    pthread_mutex_lock(&g_submit_mu);

    pthread_mutex_lock(&g_mu);
    int closing = g.closing, lost = g.device_lost, inject = g.fault_inject, failed_state = g.failed != 0;
    if (inject) g.fault_inject = 0;
    uint64_t last = g.last_signal;
    pthread_mutex_unlock(&g_mu);

    if (closing) {
        pthread_mutex_unlock(&g_submit_mu);
        finish_cmd(c, 0);
        free(s);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    // 契约 v2 的 commit：signal 必须严格大于上一次；本实现同时拒绝 0（F = 0 表示「没有失败」，
    // 无信号段失败后无法表达；见文件头消费审查第 2 条）。
    if (signal == 0 || signal <= last) {
        // 违约被拒：不提交、不推进 last_signal，但按失败处理——发布 F = 上一次合法值 + 1，
        // 再以 FAILED（rc = E_ARG）回调本段一次，signal 用调用方给的原值。
        uint64_t f = add_sat(last, 1);
        pthread_mutex_lock(&g_mu);
        g.failed = g.failed ? (f < g.failed ? f : g.failed) : f;
        pthread_mutex_unlock(&g_mu);
        s->failed = 1;
        s->rc = rc(NVN_GPU_E_ARG);
        snprintf(s->why, sizeof s->why, "signal %llu 未严格大于上一次合法提交 %llu（F=%llu）",
                 (unsigned long long)signal, (unsigned long long)last, (unsigned long long)f);
        nvn_gpu_rc result = fail(err, NVN_GPU_E_ARG, "%s", s->why);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return result;   // worker owns s after enqueue; never read its why here
    }

    if (c->in_pass) set_sticky(c, NVN_GPU_E_ORDER, "commit 时通道还开着");
    nvn_gpu_rc sticky = c->sticky;
    if (!rc_is(sticky, NVN_GPU_OK) || lost || inject || failed_state) {
        nvn_gpu_rc code = !rc_is(sticky, NVN_GPU_OK) ? sticky
                        : rc(lost ? NVN_GPU_E_DEVICE : (failed_state ? NVN_GPU_E_FAILED : NVN_GPU_E_DEVICE));
        const char *why = !rc_is(sticky, NVN_GPU_OK) ? c->why
                        : (lost ? "设备已丢失" : (failed_state ? "时间线已失败，本段不执行" : "测试注入：本段提交失败"));
        s->failed = 1;
        s->rc = code;
        snprintf(s->why, sizeof s->why, "%s", why);
        fail(err, code.code, "%s", s->why);   // preserve the first cause before worker takes ownership
        publish_signal(signal, 1);
        enqueue_segment(&g, s);
        pthread_mutex_unlock(&g_submit_mu);
        return code;
    }

    // 段末：本段动过的图一律转回 GENERAL（段与段之间的静止布局约定，见「图像布局」）。
    lay_rest_at_commit(c);
    // 段末：让本段的所有 GPU 写对主机可见（fence 只保证执行完，不保证主机可见）。
    mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
    if (c->tsq) vkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->tsq, 1);
    // 结束录制并提交。
    VkResult r = vkEndCommandBuffer(c->cb);
    if (r == VK_SUCCESS) {
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        r = vkCreateFence(g.d, &fi, NULL, &fence);
        if (r == VK_SUCCESS) {
            VkSubmitInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            VkCommandBuffer cbs[2];
            si.commandBufferCount = submit_cbs(c, cbs);
            si.pCommandBuffers = cbs;
            r = si.commandBufferCount ? vkQueueSubmit(g.queue, 1, &si, fence) : VK_ERROR_OUT_OF_HOST_MEMORY;
            if (r == VK_SUCCESS) {
                s->fence = fence;
                s->submitted = 1;
                s->executed = 1;
                publish_signal(signal, 0);
                enqueue_segment(&g, s);   // 此后 cmd 归 worker（finish_cmd 负责摘链）
                pthread_mutex_unlock(&g_submit_mu);
                return rc(NVN_GPU_OK);
            }
            vkDestroyFence(g.d, fence, NULL);
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_DEVICE, "vkQueueSubmit 失败");
        } else {
            err = err ? err : &(nvn_gpu_err){0};
            fail(err, NVN_GPU_E_OOM, "vkCreateFence 失败");
        }
    } else {
        err = err ? err : &(nvn_gpu_err){0};
        fail(err, NVN_GPU_E_DEVICE, "vkEndCommandBuffer 失败");
    }

    // 提交前失败：段不执行，按失败终止发布 F，并给本段恰好一次 FAILED 回调。
    s->failed = 1;
    s->rc = err->rc;
    snprintf(s->why, sizeof s->why, "%s", err->why);
    publish_signal(signal, 1);
    enqueue_segment(&g, s);
    pthread_mutex_unlock(&g_submit_mu);
    return err->rc;
}

nvn_gpu_rc nvn_gpu_cmd_commit(nvn_gpu_cmd ch, uint64_t signal, nvn_gpu_done_fn done, void *user, nvn_gpu_err *err) {
    if (!ch) return fail(err, NVN_GPU_E_ARG, "cmd 为空");
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许提交");
    struct nvn_gpu_cmd_s *c = ch;
    pthread_mutex_lock(&g_mu);
    if (g.closing) {   // 交给 close 的第①步统一丢弃，避免和它在同一 cmd 上重复销毁
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_STOPPED, "会话正在关闭");
    }
    struct nvn_gpu_cmd_s **pp = &g.live;
    while (*pp && *pp != c) pp = &(*pp)->live_next;
    if (!*pp) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ORDER, "cmd 已失效（abort / close 已接管）");
    }
    *pp = c->live_next;
    c->live_next = NULL;
    g.commit_active++;
    pthread_mutex_unlock(&g_mu);
    nvn_gpu_rc r = commit_impl(ch, signal, done, user, err);
    pthread_mutex_lock(&g_mu);
    g.commit_active--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t nvn_gpu_fence_done(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.completed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

uint64_t nvn_gpu_fence_failed(void) {
    if (!g_open) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g.failed;
    pthread_mutex_unlock(&g_mu);
    return v;
}

nvn_gpu_rc nvn_gpu_fence_wait(uint64_t v, uint64_t timeout_ns, nvn_gpu_err *err) {
    if (!g_open) return fail(err, NVN_GPU_E_STOPPED, "后端未打开");
    if (g_in_callback) return fail(err, NVN_GPU_E_ORDER, "回调里不许等栅栏");
    if (v == 0) return fail(err, NVN_GPU_E_ARG, "v = 0 不是提交过的值");
    pthread_mutex_lock(&g_mu);
    if (g.completed >= v) {   // 已完成的旧值不再保留在提交集合里
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_OK);
    }
    if (g.failed && v >= g.failed) {
        uint64_t f = g.failed;
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)f);
    }
    if (!submitted_has(&g, v)) {
        pthread_mutex_unlock(&g_mu);
        return fail(err, NVN_GPU_E_ARG, "v=%llu 从未提交过", (unsigned long long)v);
    }
    if (timeout_ns == 0) {
        pthread_mutex_unlock(&g_mu);
        return rc(NVN_GPU_E_TIMEOUT);
    }
    struct timespec ts;
    deadline_after(&ts, timeout_ns);
    while (g.completed < v) {
        if (g.failed && v >= g.failed) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_FAILED, "时间线首个失败段 F=%llu", (unsigned long long)g.failed);
        }
        if (g.device_lost) {
            pthread_mutex_unlock(&g_mu);
            return fail(err, NVN_GPU_E_DEVICE, "设备已丢失");
        }
        if (pthread_cond_timedwait(&g_cv, &g_mu, &ts) != 0) {
            if (g.completed < v) {
                pthread_mutex_unlock(&g_mu);
                return rc(NVN_GPU_E_TIMEOUT);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return rc(NVN_GPU_OK);
}

// ---------------------------------------------------------------- 读数 / 测试钩子

void nvn_gpu_stats(nvn_gpu_stat_fn fn, void *user, int take) {
    if (!fn || !g_open) return;
    uint64_t v[STAT_N];
    pthread_mutex_lock(&g_mu);
    memcpy(v, g.stats, sizeof v);
    if (take) memset(g.stats, 0, sizeof g.stats);
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < STAT_N; i++) fn(user, stat_names[i], v[i]);
    // 会话级读数（开会话起累计，不随 take 清零）：驱动管线缓存（#822）
    fn(user, "pipe_cache_loaded_bytes", g_plc_loaded);                              // 开会话时读到并交给驱动的字节数（0 = 没读到）
    fn(user, "pipe_cache_rejected", g_plc_rejected);                                // 旧文件读不出 / 驱动不收、被丢掉的次数
    fn(user, "pipes_created", __atomic_load_n(&g_plc_made, __ATOMIC_RELAXED));      // 经缓存建出的管线条数（图形 + 计算 + 清屏）
    // 局中补写（#868）；进程累计，不随会话清零（后台写可能跨过 close 才收尾）
    fn(user, "pipe_cache_saves", __atomic_load_n(&g_plcw_started, __ATOMIC_RELAXED));         // 交给后台写盘的次数
    fn(user, "pipe_cache_saves_ok", __atomic_load_n(&g_plcw_ok, __ATOMIC_RELAXED));           // 其中写成功的
    fn(user, "pipe_cache_saves_failed", __atomic_load_n(&g_plcw_fail, __ATOMIC_RELAXED));     // 其中写失败的（只记日志；收尾时会再试）
    fn(user, "pipe_cache_saves_same", __atomic_load_n(&g_plcw_same, __ATOMIC_RELAXED));       // 触发了但数据大小与上次保存一致、没写的次数
}

// 测试钩子：让某个 signal 的段在 fence 等待时报失败（走真实的失败终止路径）。
void nvn_gpu_vk_test_fail_fence(uint64_t signal) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_fail_signal = signal;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子：让某个 signal 的段先睡 ms，用来测 close 的 TIMEOUT 路径。
void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.test_delay_signal = signal;
    g.test_delay_ms = ms;
    pthread_mutex_unlock(&g_mu);
}

// 测试钩子（同步校验的检测器负控）：on = 0 时派发前不再按传输写补屏障，用来证明用例测得出缺屏障。只给测试用。
void nvn_gpu_vk_test_xfer_barrier(int on) { g_test_no_xfer_bar = !on; }

// 测试钩子（与 vk_clear 的 s2a_vk_clear_set_submit_fault 同一口径）：下一段 commit 按提交失败处理。
void nvn_gpu_vk_test_set_fault(int mode) {
    if (!g_open) return;
    pthread_mutex_lock(&g_mu);
    g.fault_inject = mode;
    pthread_mutex_unlock(&g_mu);
}

#include "backend_vulkan_wsi.inc"
