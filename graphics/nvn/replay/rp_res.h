// 回放核心的资源身份：描述符池登记（贴图号 / 采样器号 → 快照）、序号 → 贴图存储、同存储共用（别名）表、CPU 内容同步。
// 对齐 nvn_mtl.m：登记只存快照（@6405）；贴图在首次被回放用到时才建（tex_mtl_ @4113）；
// 同池同偏移同描述的贴图对象共用一份存储（alias_key @2386）；CPU 写的内容按脏页重传（pkg_upload_ @3202）。
#pragma once
#include "nvn_obj.h"
#include "nvn_backend.h"
#include <string.h>

// 贴图对象 → 后端贴图。首次使用时建；内容来自游戏内存的，在 c 这一段里上传 / 脏了重传。NULL = 取不到（已经 nvn_unimpl 记账）。线程：[提交]。
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c);
// 拷贝端点必须保留源格式语义，调用方已结束通道。
nvn_gpu_tex rp_tex_resolve_plain(const tex_t *t, nvn_gpu_cmd c);
void rp_bc7_note_region(nvn_gpu_cmd c, nvn_gpu_tex storage, const tex_t *t);
void rp_res_xfer_aborted(uint64_t sub);
void rp_res_byte_upload_failed(void);
extern int rp_tex_upload_diag_enabled;   // 用原子读写，首笔上传锁存会话配置
void rp_tex_upload_diag_submit(nvn_gpu_cmd cmd, uint64_t fence);   // fence=0：丢弃录制
void rp_tex_upload_diag_done(const nvn_gpu_done *done);
void rp_tex_upload_diag_window(uint64_t frames);   // frames=0：会话末窗

// 宿主整段读贴图存储的入口：整段在它的内存池里 → t->mem；伸出池子（真机：存储偏移 -1）→ 副本，池内照拷、池外按 0，*part 交调用方 free。
//   NULL = 副本分配失败。线程：任意（不取锁）。
const uint8_t *rp_tex_src(const tex_t *t, uint8_t **part);
int rp_tex_sparse_cpu(const tex_t *t);
int rp_tex_deswizzle_sparse_level(const tex_t *t, unsigned level, unsigned layer, void *linear, size_t pitch);
// 贴图号 → 视图（登记时带视图记录的按它建，否则默认视图）。号没登记 / 池外 → NULL。线程：[提交]。
// t_out 可为 NULL；非 NULL 时取到登记就在锁内把登记快照拷进调用方的 *t_out，没取到清零（serial 为 0）。
//   不交出登记表内地址：客体线程登记更大的号会让表扩容换地址（#1581）。
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, tex_t *t_out);
// 按着色器声明取兼容视图；dtt = NVN 目标 + 1，0 沿用原样。elem 表示句柄数组元素。
// 返回视图由核心持有；调用方在 pin 窗口内取用并交给后端。t_out 同 rp_view_by_id（调用方持有的快照）。线程：[提交]。
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, int elem, nvn_gpu_cmd c, tex_t *t_out);
// 句柄在游戏常量缓冲里（NVN_RB_TEXU）：号没登记（越过池容量 / 负号 / 池里没登记）按 Ryujinx 的 null 贴图绑声明类型的零贴图，不跳整笔；
// 其余同 rp_view_by_id_decl（elem = 0）。信息计数 rp_ubo_zero_count（换局清零）。
nvn_gpu_view rp_view_by_ubo_handle_decl(int id, uint8_t dtt, uint8_t dcls, nvn_gpu_cmd c, tex_t *t_out);
// 出库程序（出包预编库，声明是出包时推的）的绑定核对：begin 之后的 rp_view_by_id_decl / rp_view_by_ubo_handle_decl 遇到
//   「登记过、在池里、声明目标桥接不了」的贴图，不绑零贴图、不记未接，返回 NULL；end 返回 1；单张数值类不符返回 RP_DECL_NUMERIC_MISMATCH。调用方换现编版重绑。
//   不另取锁：判据就是取视图时那一次。线程：[提交]
enum { RP_DECL_NUMERIC_MISMATCH = 2 };   // 单张视图数值类不符，重取程序失败必须跳过该笔
void rp_decl_probe_begin(void);
int rp_decl_probe_end(void);
// 一维 / 一维数组折成二维 / 二维数组（同 iOS nvn_mtl.m tex_desc_fit，#1810）：后端建不出目标 0 / 3，翻译后的着色器也一律按二维采样
// （Sample1DAs2D）。NVN 描述里一维数组的层数放在 height，折后放进 depth、高 1；一维的高、深都是 1。后端建图、零贴图、声明与实际目标的比较都先过这一步；
// 登记快照 t->d 与客体布局 t->L 保持原样（解交织按客体的形状）。
static inline int rp_fold_1d_target(int target) {
    return target == NVN_TEXTURE_TARGET_1D ? NVN_TEXTURE_TARGET_2D :
           target == NVN_TEXTURE_TARGET_1D_ARRAY ? NVN_TEXTURE_TARGET_2D_ARRAY : target;
}
static inline void rp_fold_1d_desc(nvn_texdesc *d) {
    if (d->target == NVN_TEXTURE_TARGET_1D) { d->target = NVN_TEXTURE_TARGET_2D; d->height = 1; d->depth = 1; }
    else if (d->target == NVN_TEXTURE_TARGET_1D_ARRAY) { d->depth = d->height ? d->height : 1; d->height = 1; d->target = NVN_TEXTURE_TARGET_2D_ARRAY; }
}
static inline uint32_t rp_tex_height(const tex_t *t) {
    return rp_fold_1d_target(t->d.target) == t->d.target ? t->d.height : 1;
}
// 贴图自身的层数（不含视图记录）：立方体 6，二维数组 / 立方体数组取 depth，一维数组取 height，其余 1。
static inline uint32_t rp_tex_layer_count(const tex_t *t) {
    return t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 :
        t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t->d.depth :
        t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? t->d.height : 1;
}
// 按声明规划同一存储的视图目标；0 原样、1 已改记录、-1 无法桥接。不会创建零贴图。
static inline int rp_decl_view_target(const tex_t *t, view_t *v, int has_v, uint8_t dtt) {
    if (!dtt) return 0;
    int want = rp_fold_1d_target(dtt - 1), have = rp_fold_1d_target(has_v && v->set_target ? v->target : t->d.target);
    if (want == have) {
        if (want != NVN_TEXTURE_TARGET_CUBEMAP && want != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) return 0;
        const uint32_t total = rp_tex_layer_count(t);
        const int explicit_layers = has_v && v->set_layers;
        const uint32_t base = explicit_layers ? v->min_layer : 0;
        const uint32_t count = explicit_layers ? (v->nlayers ? v->nlayers : 1) : total;
        // 后端可以把不完整的立方层范围当二维视图用，采样绑定必须保留着色器的声明维度。
        if (base >= total || count > total - base || count < 6 || count % 6) return -1;
        if (want == NVN_TEXTURE_TARGET_CUBEMAP) return count == 6 ? 0 : -1;
        // 单组六面默认会落成立方体；显式指定目标才能保留一组立方体的数组视图。
        if (count != 6 || (has_v && v->set_target)) return 0;
        if (!has_v) memset(v, 0, sizeof *v);
        v->set_target = 1; v->target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        return 1;
    }
    uint32_t layers = rp_tex_layer_count(t);
    if (has_v && v->set_layers) layers = v->nlayers ? v->nlayers : (layers > v->min_layer ? layers - v->min_layer : 1);
    uint32_t n = 0;
    int layered = have == NVN_TEXTURE_TARGET_2D || have == NVN_TEXTURE_TARGET_2D_ARRAY ||
        have == NVN_TEXTURE_TARGET_CUBEMAP || have == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if (layered) {
        if (want == NVN_TEXTURE_TARGET_2D) n = 1;
        else if (want == NVN_TEXTURE_TARGET_2D_ARRAY) n = layers;
        else if (want == NVN_TEXTURE_TARGET_CUBEMAP && layers >= 6) n = 6;
        else if (want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && layers >= 6) n = layers / 6 * 6;
    }
    if ((want == NVN_TEXTURE_TARGET_2D_MULTISAMPLE && have == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY) ||
        (want == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY && have == NVN_TEXTURE_TARGET_2D_MULTISAMPLE)) n = 1;
    if (!n || n > UINT16_MAX) return -1;
    int cube = want == NVN_TEXTURE_TARGET_CUBEMAP || want == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    int cube_compat_array = t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY && t->d.width == t->d.height && t->d.samples <= 1 && t->d.depth >= 6;
    if (cube && t->d.target != NVN_TEXTURE_TARGET_CUBEMAP && t->d.target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !cube_compat_array) return -1;
    if (!has_v) memset(v, 0, sizeof *v);
    v->set_target = 1; v->target = (uint8_t)want;
    v->set_layers = 1; v->nlayers = (uint16_t)n;
    return 1;
}
uint64_t rp_empty_slot_zero_count(void);   // 空贴图槽绑零贴图的次数（#1147；信息计数）
uint64_t rp_ubo_zero_count(void);          // 常量缓冲句柄指向没登记的号、绑零贴图的次数（#1594 片 4；信息计数，换局清零）
// 贴图对象 + 视图记录（NULL = 默认视图）→ 视图：渲染目标、拷贝端点用。线程：[提交]。
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c);
nvn_gpu_view rp_view_for_plain(const tex_t *t, const view_t *v, nvn_gpu_cmd c);
// 当颜色附件取视图。刚建、初始内容该是全零的单级单层颜色图：不在 CPU 上填零，记成「待清零」，
// 由调用方在开通道前用 rp_tex_take_zero 取走并把这个附件的载入方式改成清零（显卡做，CPU 不搬一整张零）。
// 没被取走的待清零，下一次任何别的用途取它时照旧填零。
nvn_gpu_view rp_view_for_attach(const tex_t *t, const view_t *v, nvn_gpu_cmd c);
// 需要换后端存储的附件应在本笔取得采样/存储视图之前准备。
int rp_tex_prepare_sparse_attachment(const tex_t *t, nvn_gpu_cmd c);
int rp_tex_take_zero(const tex_t *t);   // 1 = 这张图待清零，已取走：调用方必须让它在这个通道里整张清成零
// 上屏用的默认视图：不经可淘汰的视图缓存（上屏线程与提交线程共用一份环会互相淘汰；
// iOS 的 present 直接拿贴图对象，没有这层缓存）。调用方用完即 release，后端段按引用保活。
nvn_gpu_view rp_present_view(const tex_t *t);
// 一段取视图的窗口：窗口内取到的缓存项不会被轮转淘汰（一笔通道要同时挂好几个附件，
// 取第二个的时候不能把第一个已经从缓存里拿到、还没交给后端的视图放掉）。
void rp_view_pin_begin(void);
void rp_view_pin_end(void);
// 贴图号 → 登记时的贴图 / 视图快照（不建图）。1 = 这个号登记过。
int rp_tex_snap_by_id(int id, tex_t *t, view_t *v, int *has_v);
// 存储图像的 sRGB 别名：后端置了 NVN_GPU_CAP_STORAGE_SRGB_ALIAS、格式 f 本身不能当存储、同位宽 UNORM 能，返回那个 UNORM 格式；否则 0。
//   NVN 的图像存储按原始字节写，不做 sRGB 编码，所以按 UNORM 视图写进 sRGB 贴图是同一份字节。
uint32_t rp_storage_srgb_alias(uint32_t f);
// 采样器号 → 后端采样器（首次取用时建）。没登记 → NULL。线程：[提交]。
nvn_gpu_samp rp_samp_by_id(int id);
// #1072：同上，但贴图 tid 的有效格式不能线性过滤时给同参数的最近点采样器（具名记账）。
nvn_gpu_samp rp_samp_for_tex(int sid, int tid);
// 类型 3 句柄（常量采样器；c2 分离表 / 游戏常量缓冲两种形态）的采样器：登记号 sid 没登记 → 同 Ryujinx 的空采样器，绑最近点 / Repeat 的
// 默认采样器（rp_sampler_for_query）、不跳整笔；其余同 rp_samp_for_tex。登记了但后端建不出 / 默认采样器也建不出 → NULL（调用方具名跳过）。
// pool_base / pool_n（采样器池起点、容量）只给每局首行日志用。信息计数 rp_const_samp_default_count（换局清零）。线程：[提交]。
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pool_base, int pool_n);
uint64_t rp_const_samp_default_count(void);   // 类型 3 句柄的采样器号没登记、绑默认采样器的次数（#1594 片 6；信息计数，换局清零）
// 格式能力（nvn_gpu_format_caps）按格式缓存，会话重开时清。
uint32_t rp_fmt_caps(uint32_t f);
// 仅供已证明不读取采样器的组合描述符；Vulkan 仍要求描述符里的对象有效。
nvn_gpu_samp rp_sampler_for_query(void);
// 整池数组（NVN_RB_TEXP / SAMPP）的池快照：按键缓存、登记纪元变了才重建；NULL = 建不出，*why 是具名跳过的原因。线程：[提交]，开通道前
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c, int samp, int base, uint32_t n, uint8_t dtt, uint8_t dcls, const char **why);
// 这张贴图的内容从此以 GPU 为准（当了渲染目标 / 被拷贝写 / 被着色器写）：不再从游戏内存重传。
void rp_tex_mark_gpu_written(const tex_t *t);
// 诊断纯清除通道裁掉的附件身份，关联后继上传。调用方检查诊断开关。
void rp_tex_note_co_trim(const tex_t *t);
// 只读查询：这个序号被前端标过「CPU 写过」（nvn_be_tex_cpu_written：WriteTexels、刷新范围盖到、导出等）。同步读回判内容在不在游戏内存里用。线程：任意。
int rp_tex_cpu_written(uint64_t serial);
// 调试：序号 → 后端贴图（没建过 = NULL）。
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t serial);
// 本笔已解析的绑定是否会访问底图。序号尚未建图时也查可复用的共用存储，不分配、不上传。
struct rp_pass_read;
int rp_tex_is_read(const tex_t *t, const struct rp_pass_read *reads, int n);
// 不同对象是否按资源表规则共用一张底图；两者尚未建图时也按相同的别名准入判断。
int rp_tex_storage_aliases(const tex_t *a, const tex_t *b);
// 会话收尾：放掉全部后端对象、清表。
void rp_res_reset(void);
// 读数（单测 / 内存账）
int rp_res_tex_built(void);      // 已建的后端贴图个数
int rp_res_tex_serials(void);    // 已解析的贴图对象个数（多个对象可共用一份存储）
uint64_t rp_res_uploads(void);   // 累计上传次数（每张贴图每次同步算一次）
// 前端按名字调、但上沿头文件里没有声明的两个入口（hle_nvn.c 里就地 extern）
void nvn_be_pool_gpu_written(const void *pool);                    // 这个池里录到了 GPU 写
int nvn_be_tex_gpu_owned_since(uint64_t serial, uint64_t stamp);   // 脏页轮询：这张贴图的内容自 stamp 起是否归 GPU
int rp_tex_debug_state(uint64_t serial);   // 调试取证（逐笔日志用）
uint64_t rp_tex_reg_gen(void);   // 贴图登记表的代号：登记 / 注销 / 清空各加一
uint64_t rp_tex_fact_sig(const uint64_t *handles, int n, int tex_base, int tex_pool_n);   // 一组贴图槽的编译事实摘要（着色器来源的快路径用）
