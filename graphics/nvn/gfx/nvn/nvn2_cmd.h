// NVN2（Switch 2）新增的两类命令缓冲记录与 DLSS 对象布局：推常量（nvnCommandBufferPush{Graphics,Compute}Constants）、DLSS 求值（nvnCommandBufferEvaluateDlss）。
// 录制在 hle_nvn.c，回放在 nvn_mtl.m（DLSS → MetalFX 时域放大，nvn_dlss.inc）。纯 C，单测 graphics/nvn/tests/t_nvn2.c 直接测这里的布局与换算。
// 操作码接在 nvn_obj.h 的 OP_COUNT_ 之后、不写进 nvn_obj.h：nvn_obj.h 是直连出库阶段键的输入（pack/stage_inputs/direct.tsv「入」），改它让每款直连库冷重出，
//   这两条只有宿主录、宿主放，与出库产物无关。按 OP_COUNT_ 起算：nvn_obj.h 以后加操作码，这里跟着后移，不会撞号。
// 事实来源：kartnative（MIT）docs/reference/nvn2.md「Dlss」「Push constants」两节（马车世界 SDK 19.3.3 反汇编）；火纹 Fortune's Weave 实测调用（11-03 真机 host.log）。代码自写
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn_obj.h"

// OP_PUSH_CONST_PTR（11-04）：Push{Graphics,Compute}ConstantsDeferred——数据指针录下、回放时才读（见下「推常量」）。新操作码一律接在 OP_NVN2_END_ 前，回放 rec_bad 的上限跟着走
// OP_MULTI_DRAW（11-06）：间接计数多绘超过 1,024 条时整段一条记录（见下「间接计数多绘」；补齐上游参考实现（MIT）c7ba23df，上游放在 nvn_obj.h，这里同理接在 OP_COUNT_ 之后）
enum { OP_PUSH_CONST = OP_COUNT_, OP_DLSS, OP_PUSH_CONST_PTR, OP_MULTI_DRAW, OP_SET_PROVOKING_VERTEX, OP_SAMPLE_MASK, OP_DISCARD_COLOR, OP_DISCARD_DS, OP_NVN2_END_ };

// NVN 驱动参数按硬件位取值，0 取首顶点、1 取末顶点。
typedef struct { uint32_t mode; } p_provoking_vertex;
typedef struct { uint32_t mask; } p_sample_mask;
typedef struct { int32_t slot; uint32_t pad; } p_discard_color;

// ---------- 间接计数多绘（MultiDraw{Arrays,Elements}IndirectCount）----------
// 不超过 1,024 条的照旧逐条录成带计数地址与序号的间接绘制（hle_nvn.c rec_draw / rec_draw_idx，怪物猎人崛起那条路不变）；更大的一次调用录成这一条：参数记录的起点（宿主地址）、
//   有符号步长、GPU 写的计数（u32）地址、最多条数、图元与索引类型（DrawArrays 记 -1）、索引缓冲。回放 draw_multi（nvn_mtl.m）按 GPU 掩码分批画、CPU 不等；
//   步长 0、要先转换的（四边形 / 扇形 / 8 位索引）与超过掩码表容量的，读一次计数只画活着的那些。上游症状：MIO 的 GPU 驱动场景（最多 4,594 / 8,192 条）第 1,024 条以后全丢
typedef struct { uint64_t ind, cnt, ibuf; int64_t stride; int32_t max, prim, itype, _pad; } p_multi_draw;

// ---------- 推常量 ----------
// PushGraphicsConstants(cmd, slot, offset, size, data)：Ampere 的 SET_ROOT_TABLE_SELECTOR / LOAD_ROOT_TABLE，图形各阶段把槽 k 读成常量库 c[24 + k]；
//   槽号取低 3 位，字节数夹到 256、不足 4 字节不发（驱动 sdk+0x5d34b0）。PushComputeConstants 同参数，计算着色器的推常量区（c0 里的那段）由翻译器映到 c[24]
//   （Ryujinx.Graphics.Shader InstEmitAmpere.cs AMapC0），宿主同样按槽放。数据录制时拷进命令内存（游戏可以随后改原数据）
enum { NVN2_PUSH_SLOTS = 8, NVN2_PUSH_BYTES = 256, NVN2_PUSH_BANK = 24 };
typedef struct { uint8_t compute, slot; uint16_t offset; uint32_t size; } p_push_const;   // 后跟 size 字节数据
// 延后版 Push{Graphics,Compute}ConstantsDeferred(cmd, 槽, 偏移, 字节数, 数据指针)：参数同上（槽 / 偏移各存 1 字节），驱动只在控制内存里记一条
//   {令牌 0x1e / 0x1f, 槽, 偏移, 字节数, 指针}（火纹 SDK 22 sdk+0x673810 / +0x673cc0），队列处理这条命令缓冲时（sdk+0x665220 的令牌分派 → +0x6738a0）才按指针读数据、
//   照立即版同样规整（< 4 字节不发、夹到 256、槽取低 3 位）发出去。宿主同义：录下指针，回放（提交时）再读。规整在录制时做（与立即版共用 nvn2_push_range）
typedef struct { uint8_t compute, slot; uint16_t offset; uint32_t size; uint64_t data; } p_push_const_ptr;
_Static_assert(__builtin_offsetof(p_push_const_ptr, size) == __builtin_offsetof(p_push_const, size), "回放按 p_push_const 读前四项");
// 规整参数：返回 0 = 不发（驱动不足 4 字节不发）；否则 *slot / *off / *n 是落进 256 字节槽内的范围
static inline int nvn2_push_range(int slot, int offset, int size, unsigned *s, unsigned *off, unsigned *n) {
    if (size < 4) return 0;
    *s = (unsigned)slot & (NVN2_PUSH_SLOTS - 1);
    *off = (unsigned)offset & 0xff;
    unsigned m = (unsigned)size > NVN2_PUSH_BYTES ? NVN2_PUSH_BYTES : (unsigned)size;
    if (*off + m > NVN2_PUSH_BYTES) m = NVN2_PUSH_BYTES - *off;
    *n = m;
    return m > 0;
}

// ---------- DLSS 对象（游戏分配内存，宿主按驱动的布局读写，Get 读回同一处）----------
// NVNdlssNetworkBuilder 0xC0：+0x00 设备、+0x08 纹理池、+0x10/+0x18 存储与大小、+0x20 标志、+0x24 方法、+0x28 预设、+0x2c/+0x30 输出宽高、+0x34 最多求值条目（缺省 1）、
//   +0x38 纹理号 int[32]。SetDefaults 写 +0x08..+0xbf（设备不清）
enum { DLB_DEVICE = 0x00, DLB_POOL = 0x08, DLB_MEM = 0x10, DLB_MEMSZ = 0x18, DLB_FLAGS = 0x20, DLB_METHOD = 0x24, DLB_PRESET = 0x28, DLB_OUT_W = 0x2c, DLB_OUT_H = 0x30,
       DLB_MAXENT = 0x34, DLB_IDS = 0x38, DLB_SIZE = 0xc0 };
// NVNdlssNetwork 0x218：Initialize 整块清零后填。驱动的 +0x08 设备、+0x10 纹理池、+0x18 纹理号、+0x1c8 最多条目、+0x1d4 方法、+0x1e0 预设、+0x1ec/+0x1f0 输出宽高、
//   +0x210 构建标志；后端句柄（+0xa0）宿主用不到，写一个非 0 标记表示已初始化
//   DLN_MAX_W / H（宿主自用的空位）：Initialize 时的输出分辨率 = Resize 的上限（驱动 20.5 在 +0x210 / +0x214 记它，宿主那里放了构建标志，挪到空着的 +0x1f4 / +0x1f8）
enum { DLN_DEVICE = 0x08, DLN_POOL = 0x10, DLN_IDS = 0x18, DLN_HANDLE = 0xa0, DLN_MAXENT = 0x1c8, DLN_METHOD = 0x1d4, DLN_PRESET = 0x1e0, DLN_OUT_W = 0x1ec, DLN_OUT_H = 0x1f0,
       DLN_MAX_W = 0x1f4, DLN_MAX_H = 0x1f8, DLN_FLAGS = 0x210, DLN_SIZE = 0x218 };
// NVNdlssParameters 0xC8：(贴图, 视图) 对在 +0x00 输入色、+0x10 输出色、+0x20 运动向量、+0x30 深度、+0x40 偏置掩码、+0x68 曝光；+0x50 抖动 xy、+0x58 运动向量缩放 xy（缺省 1）、
//   +0x60 预曝光（缺省 1）、+0x78 历史重置、+0x7c 求值序号、+0x80 输入子矩形大小（0 = 输入贴图大小）、+0x88/+0x90/+0x98/+0xa0/+0xa8 输入色 / 深度 / 运动 / 偏置 / 输出子矩形起点、
//   +0xb0..+0xbc 叠加调试四项（缺省 -1）。SetDefaults 写满 0..0xc7
enum { DLP_COLOR = 0x00, DLP_OUTPUT = 0x10, DLP_MOTION = 0x20, DLP_DEPTH = 0x30, DLP_BIAS = 0x40, DLP_JITTER = 0x50, DLP_MVSCALE = 0x58, DLP_PREEXP = 0x60, DLP_EXPOSURE = 0x68,
       DLP_RESET = 0x78, DLP_INDEX = 0x7c, DLP_INSIZE = 0x80, DLP_BASE_COLOR = 0x88, DLP_BASE_DEPTH = 0x90, DLP_BASE_MOTION = 0x98, DLP_BASE_BIAS = 0xa0, DLP_BASE_OUTPUT = 0xa8,
       DLP_OVERLAY = 0xb0, DLP_SIZE = 0xc8 };
// 构建标志（NGX 的特性位：第 0 位取反成 HDR，第 1、2、3、6 位照传）：1 运动向量是渲染分辨率、2 运动向量含抖动、3 深度反向（近 1 远 0）、6 自动曝光
enum { DLF_LDR = 1u << 0, DLF_MV_LOWRES = 1u << 1, DLF_MV_JITTERED = 1u << 2, DLF_DEPTH_INVERTED = 1u << 3, DLF_AUTO_EXPOSURE = 1u << 6 };

static inline void nvn2_put32(void *o, unsigned at, uint32_t v) { memcpy((uint8_t *)o + at, &v, 4); }
static inline uint32_t nvn2_get32(const void *o, unsigned at) { uint32_t v; memcpy(&v, (const uint8_t *)o + at, 4); return v; }
static inline void nvn2_putf(void *o, unsigned at, float v) { memcpy((uint8_t *)o + at, &v, 4); }
static inline float nvn2_getf(const void *o, unsigned at) { float v; memcpy(&v, (const uint8_t *)o + at, 4); return v; }
static inline void nvn2_putp(void *o, unsigned at, const void *p) { memcpy((uint8_t *)o + at, &p, 8); }
static inline const void *nvn2_getp(const void *o, unsigned at) { const void *p; memcpy(&p, (const uint8_t *)o + at, 8); return p; }

static inline void nvn2_dlss_builder_defaults(void *b) { memset((uint8_t *)b + DLB_POOL, 0, DLB_SIZE - DLB_POOL); nvn2_put32(b, DLB_MAXENT, 1); }
static inline void nvn2_dlss_params_defaults(void *p) {
    memset(p, 0, DLP_SIZE);
    nvn2_putf(p, DLP_MVSCALE, 1.0f); nvn2_putf(p, DLP_MVSCALE + 4, 1.0f); nvn2_putf(p, DLP_PREEXP, 1.0f);
    memset((uint8_t *)p + DLP_OVERLAY, 0xff, 16);
}

// 放大比例（NGX 的性能 / 质量档 → 渲染尺寸占输出的比例）：0 性能 1/2、1 均衡 0.58、2 质量 2/3、3 超性能 1/3、4 超质量 0.77、5 DLAA 1
static inline double nvn2_dlss_ratio(int method) {
    static const double r[] = {0.5, 0.58, 2.0 / 3.0, 1.0 / 3.0, 0.77, 1.0};
    return method >= 0 && method < (int)(sizeof r / sizeof *r) ? r[method] : 0.5;
}
// GetOptimalSettings 的六个输出（NGX 次序：最佳宽高、最大宽高、最小宽高）。宿主的放大器（MetalFX 时域放大）放大倍数有上限 max_scale：
//   最小渲染尺寸 = 输出 ÷ 上限（向上取整），最大 = 输出，最佳 = 输出 × 档位比例，夹在两者之间
static inline void nvn2_dlss_optimal(int out_w, int out_h, int method, double max_scale, int o[6]) {
    double r = nvn2_dlss_ratio(method), m = max_scale >= 1.0 ? max_scale : 1.0;
    int minw = (int)((out_w + m - 1e-9) / m), minh = (int)((out_h + m - 1e-9) / m);
    if (minw * m < out_w) minw++;
    if (minh * m < out_h) minh++;
    int w = (int)(out_w * r + 0.5), h = (int)(out_h * r + 0.5);
    if (w < minw) w = minw;
    if (h < minh) h = minh;
    if (w > out_w) w = out_w;
    if (h > out_h) h = out_h;
    o[0] = w; o[1] = h; o[2] = out_w; o[3] = out_h; o[4] = minw; o[5] = minh;
}

// DLSS 求值记录：六张贴图按 Parameters 的次序快照（同 nvn_obj.h 各录制的 tex_snap：录完游戏可以改 / 释放对象），其余参数与网络的输出尺寸、标志在录制时取定
enum { DLT_COLOR, DLT_OUTPUT, DLT_MOTION, DLT_DEPTH, DLT_BIAS, DLT_EXPOSURE, DLT_N };
typedef struct {
    const tex_t *tex[DLT_N]; view_t view[DLT_N]; uint8_t has_view[DLT_N]; uint8_t reset, _pad;
    float jitter[2], mv_scale[2], pre_exposure;
    int32_t in_w, in_h;          // 输入子矩形大小（0 = 输入贴图大小）
    int32_t base[5][2];          // 子矩形起点：输入色、深度、运动、偏置、输出
    int32_t out_w, out_h;        // 网络的输出分辨率（SetOutputResolution）
    uint32_t flags; int32_t method, index;
    uint64_t network;            // 网络对象地址：放大器每个网络一个（历史随网络，求值序号不分历史，见 nvn_dlss.inc）
} p_dlss;
static const unsigned nvn2_dlss_tex_at[DLT_N] = {DLP_COLOR, DLP_OUTPUT, DLP_MOTION, DLP_DEPTH, DLP_BIAS, DLP_EXPOSURE};

// ---------- NVN2 新增对象 / 新增字段（11-04 入口全量；hle_nvn.c「NVN2 入口全量」段读写）----------
// 地址一律是火纹 Fortune's Weave（SDK 22）sdk 模块里驱动的函数（graphics/nvn/tools/nvn2_procs.py 抽的表）；金刚 / Pokopia（20.5）、Raiders（21.4）同名函数同布局。
// NVN2 对象大小：驱动写到哪里是下界；上界取自 pokopia-recomp nvn2_abi.md §3（游戏自己的栈槽 / 包装类跨度，事实取自 pokopia-recomp（GPL，代码自写））。
// 宿主字段放法：纯新对象（程序构建器、设备命令缓冲、设备消息缓冲、脏块图参数）照驱动布局；NVN1 已有、宿主已有自己布局的对象（窗口 / 窗口构建器 / 队列构建器 /
//   多重采样状态）新字段接在宿主结构之后（不改 nvn_obj.h：它进直连库阶段键），且只在 Switch 2 上写（Switch 1 的对象按 NVN1 大小分配）
#define FITS_N(T, n) _Static_assert(sizeof(T) <= (n), #T " exceeds the NVN2 object size")

// NVNprogramBuilder（SetDefaults 0x660d40 写 +0x18..+0x63）：+0x08 设备、+0x18 逐样本着色、+0x1c/+0x20 调试数据哈希（个数, 指针）、+0x28/+0x30 阶段（个数, NVNshaderData[]）、
//   +0x38/+0x40 光追模块（个数, 指针）、+0x48/+0x50 子程序链接（个数, 指针）、+0x58 最大追踪递归深度（缺省 2）、+0x5c 最大可调用递归深度（缺省 2）、+0x60 光追程序标志。
//   SetDefaults 不动 +0x00..+0x17（设备照旧）
enum { PB_DEVICE = 0x08, PB_SAMPLE_SHADING = 0x18, PB_DBG_N = 0x1c, PB_DBG = 0x20, PB_NSHADER = 0x28, PB_SHADERS = 0x30, PB_NRT = 0x38, PB_RT = 0x40,
       PB_NSUB = 0x48, PB_SUB = 0x50, PB_MAX_TRACE = 0x58, PB_MAX_CALLABLE = 0x5c, PB_RT_FLAGS = 0x60, PB_SIZE = 0x64 };
static inline void nvn2_prog_builder_defaults(void *b) {
    memset((uint8_t *)b + PB_SAMPLE_SHADING, 0, PB_SIZE - PB_SAMPLE_SHADING);
    nvn2_put32(b, PB_MAX_TRACE, 2); nvn2_put32(b, PB_MAX_CALLABLE, 2);
}
// NVNdeviceCommandBuffer（SetDefaults 0x656850 清 +0x08..+0x2f）：+0x00 设备、+0x08 存储池、+0x10 池内偏移、+0x18 大小、+0x20 命令内存大小、+0x28 计算内存大小
enum { DCB_DEVICE = 0x00, DCB_POOL = 0x08, DCB_OFF = 0x10, DCB_SIZE_ = 0x18, DCB_CMD = 0x20, DCB_COMP = 0x28, DCB_SIZE = 0x30 };
// NVNdeviceMessageBuffer（SetDefaults 0x6570d0 清 +0x08..+0x2f）：+0x00 设备、+0x08 存储池、+0x10 池内偏移（int）、+0x14 大小（int）、+0x18 读游标基址、+0x20 当前读位置、+0x24 读的终点。
//   存储开头是 GPU 写的头：{u32 首条消息偏移 = 12, u32 容量, u32 已写到的偏移}（ResetDeviceMessageBuffer / DeviceMessageBufferReset 写 {12, 大小, 0}）
enum { DMB_DEVICE = 0x00, DMB_POOL = 0x08, DMB_OFF = 0x10, DMB_CAP = 0x14, DMB_BASE = 0x18, DMB_CUR = 0x20, DMB_END = 0x24, DMB_SIZE = 0x28, DMB_HDR = 12 };
// NVNdirtyTileMapParameters（SetDefaults 0x64ed40，Get 读到 +0x73）：+0x00/+0x08/+0x10 贴图、视图、24 字节区域；+0x28/+0x5c/+0x60/+0x64 输入四项；+0x2c 常量色表项数；
//   +0x30/+0x38/+0x3c/+0x40 输出四项；+0x48/+0x50 常量色表（指针, 第四项）、+0x58 常量色表第三项；+0x68/+0x6c 深度 / 模板掩码；+0x70..+0x73 扩展足迹四个字节。
//   缺省全 0，只有 +0x58..+0x67 来自一个 16 字节常量（火纹 sdk 0xbb0f80：+0x58 = 1、+0x5c = 16、+0x60 = 16、+0x64 = 0）
enum { DTM_TEX = 0x00, DTM_VIEW = 0x08, DTM_REGION = 0x10, DTM_IN_B = 0x28, DTM_CC_N = 0x2c, DTM_OUT0 = 0x30, DTM_OUT1 = 0x38, DTM_OUT2 = 0x3c, DTM_OUT3 = 0x40,
       DTM_CC = 0x48, DTM_CC_D = 0x50, DTM_CC_C = 0x58, DTM_IN_C = 0x5c, DTM_IN_D = 0x60, DTM_IN_A = 0x64, DTM_DSMASK = 0x68, DTM_FOOT = 0x70, DTM_SIZE = 0x74 };
static inline void nvn2_dtm_defaults(void *p) {
    memset(p, 0, DTM_SIZE);
    nvn2_put32(p, DTM_CC_C, 1); nvn2_put32(p, DTM_IN_C, 16); nvn2_put32(p, DTM_IN_D, 16);
}

// 窗口构建器的 NVN2 字段（驱动 +0x28 获取超时（缺省 -1 = 一直等）、+0x30 色彩空间、+0x40..+0x67 HDR 显示元数据十个 float、+0x68/+0x6c HDR 内容元数据两个 float），
//   宿主接在 win_builder_t（0x40）之后。NVNwindowBuilder：NVN1 0x40，NVN2 0x80（驱动写到 +0x6f）
typedef struct { win_builder_t b; int64_t acquire_timeout; int32_t color_space; float hdr_display[10], hdr_content[2]; } win_builder2;   FITS_N(win_builder2, 0x80);
// 窗口的 NVN2 字段（驱动 +0x30 获取超时、+0x48..+0x6f 显示元数据、+0x70/+0x74 内容元数据），接在 win_t 之后。NVNwindow：NVN1 0x180，NVN2 0x218..0x280
typedef struct { win_t w; int64_t acquire_timeout; int32_t color_space; float hdr_display[10], hdr_content[2]; } win2;                    FITS(win2, NVNwindow);
// HDR 元数据缺省（WindowBuilderSetDefaults 0x66c240 从 0xbb47f0 / 0xbb2a70 / 0xbb5140 三个 16 字节常量装 +0x40..+0x6f）：显示 = Rec.709 三原色 R(0.64, 0.33) G(0.30, 0.60)
//   B(0.15, 0.06)、D65 白点 (0.3127, 0.329)、最暗 0.03、最亮 1000 尼特；内容 = 最大单像素亮度 200、最大帧平均亮度 60
static const float nvn2_hdr_display_def[10] = {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.329f, 0.03f, 1000.0f};
static const float nvn2_hdr_content_def[2] = {200.0f, 60.0f};
// 队列构建器的 NVN2 字段：主队列（SetPrimaryQueue，驱动 +0x18）、光追内存大小与「设过」（驱动 +0x38 / +0x27）
typedef struct { queue_builder_t b; NVNqueue *primary; int32_t rt_mem; uint8_t rt_set; } queue_builder2;                              FITS(queue_builder2, NVNqueueBuilder);
// 多重采样状态的 NVN2 字段：覆盖截断（驱动字 0 第 5 位）。接在 ms_st（10 字节）之后；绑定只录 ms_st，回放不看它
typedef struct { ms_st s; uint8_t cov_trunc; } ms2;                                                                                       FITS(ms2, NVNmultisampleState);

// ---------- 任务 / 网格着色（11-05，nvnCommandBufferDrawMeshTasks / DrawMeshTasksIndirect；约定见 Switch2 兼容线「网格着色接口约定」） ----------
// 录成 OP_DRAW、图元号取 DM_PRIM_MESH_TASKS（NVN 没有这个图元值）：p_draw.first / count = 起始工作组号 / 工作组数（有任务着色器时是任务工作组，没有时是网格工作组）；
//   间接版 count = first = 0、ind = GPU 写的 {u32 工作组数, u32 起始号} 的地址（回放 nvn_geometry.inc mesh_tasks_draw）。用 OP_DRAW 而不另起操作码：
//   前瞻（存取索引 sdx_walk、推迟 / 合并判定）把它当一笔画到当前目标的绘制看待，这正是它在渲染遍里的效果。参照上游参考实现（MIT） 1d4b8e82b / #1312（MIT，只取做法）
enum { DM_PRIM_MESH_TASKS = 0x40 };
// NVN2 的任务 / 网格阶段（NVNshaderStage 6 / 7）放在宿主曲面细分两阶段的位置 3 / 4：一个程序要么有曲面要么有网格（硬件两条管线互斥），回放的绑定状态保持 6 个阶段
static inline int nvn2_stage_host(int st) { return st == 6 ? 3 : st == 7 ? 4 : st; }
// BindProgram 的阶段位：任务 0x40 → 0x08、网格 0x80 → 0x10（同上）
static inline int nvn2_stage_bits(int bits) { return (bits & 0x3f) | (bits & 0x40 ? 0x08 : 0) | (bits & 0x80 ? 0x10 : 0); }
