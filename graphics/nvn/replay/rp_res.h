// 回放核心的资源身份：描述符池登记（贴图号 / 采样器号 → 快照）、序号 → 贴图存储、同存储共用（别名）表、CPU 内容同步。
// 对齐 nvn_mtl.m：登记只存快照（@6405）；贴图在首次被回放用到时才建（tex_mtl_ @4113）；
// 同池同偏移同描述的贴图对象共用一份存储（alias_key @2386）；CPU 写的内容按脏页重传（pkg_upload_ @3202）。
#pragma once
#include "nvn_obj.h"
#include "nvn_backend.h"
#include <string.h>

// 贴图对象 → 后端贴图。首次使用时建；内容来自游戏内存的，在 c 这一段里上传 / 脏了重传。NULL = 取不到（已经 nvn_unimpl 记账）。线程：[提交]。
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c);
// 贴图号 → 视图（登记时带视图记录的按它建，否则默认视图）。t_out 可为 NULL。号没登记 / 池外 → NULL。线程：[提交]。
nvn_gpu_view rp_view_by_id(int id, nvn_gpu_cmd c, const tex_t **t_out);
// 按着色器声明取兼容视图；dtt = NVN 目标 + 1，0 沿用原样。elem 表示句柄数组元素。
// 返回视图由核心持有；调用方在 pin 窗口内取用并交给后端。线程：[提交]。
nvn_gpu_view rp_view_by_id_decl(int id, uint8_t dtt, uint8_t dcls, int elem, nvn_gpu_cmd c, const tex_t **t_out);
// 按声明规划同一存储的视图目标；0 原样、1 已改记录、-1 无法桥接。不会创建零贴图。
static inline int rp_decl_view_target(const tex_t *t, view_t *v, int has_v, uint8_t dtt) {
    if (!dtt) return 0;
    int want = dtt - 1, have = has_v && v->set_target ? v->target : t->d.target;
    if (want == have) return 0;
    uint32_t layers = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 :
        t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t->d.depth : 1;
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
// 贴图对象 + 视图记录（NULL = 默认视图）→ 视图：渲染目标、拷贝端点用。线程：[提交]。
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c);
// 当颜色附件取视图。刚建、初始内容该是全零的单级单层颜色图：不在 CPU 上填零，记成「待清零」，
// 由调用方在开通道前用 rp_tex_take_zero 取走并把这个附件的载入方式改成清零（显卡做，CPU 不搬一整张零）。
// 没被取走的待清零，下一次任何别的用途取它时照旧填零。
nvn_gpu_view rp_view_for_attach(const tex_t *t, const view_t *v, nvn_gpu_cmd c);
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
// 格式能力（nvn_gpu_format_caps）按格式缓存，会话重开时清。
uint32_t rp_fmt_caps(uint32_t f);
// 这张贴图的内容从此以 GPU 为准（当了渲染目标 / 被拷贝写 / 被着色器写）：不再从游戏内存重传。
void rp_tex_mark_gpu_written(const tex_t *t);
// 只读查询：这个序号被前端标过「CPU 写过」（nvn_be_tex_cpu_written：WriteTexels、刷新范围盖到、导出等）。同步读回判内容在不在游戏内存里用。线程：任意。
int rp_tex_cpu_written(uint64_t serial);
// 调试：序号 → 后端贴图（没建过 = NULL）。
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t serial);
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
