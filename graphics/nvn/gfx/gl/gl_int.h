// OpenGL 前端（直连，WP7）各分片共用的头：类型、函数登记宏、上下文状态、对象表。接口约定见 docs/研究/GL前端接口.md；
//   改这个文件先在 #272 提（各片都依赖它）。每片只改自己的 .m：egl.m / gl_buf.m / gl_tex.m / gl_prog.m / gl_fb.m / gl_draw.m
#pragma once
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#import <Metal/Metal.h>
#include <stdatomic.h>

// ---------- 类型（GL 4.5 core / GLES 3.2、EGL 1.5 的 C 类型；ARM64 上与 Switch 的 ABI 一致） ----------
typedef uint32_t GLenum, GLuint, GLbitfield; typedef int32_t GLint, GLsizei; typedef uint8_t GLboolean; typedef float GLfloat, GLclampf;
typedef double GLdouble; typedef char GLchar; typedef intptr_t GLintptr, GLsizeiptr; typedef uint64_t GLuint64; typedef struct __GLsync *GLsync;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLNativeWindowType; typedef int32_t EGLint; typedef uint32_t EGLBoolean, EGLenum;
// 注意（hle.h 文件头）：Switch 调用方不扩展 < 32 位的参数，GLboolean 之类的参数在实现里一律声明成 GLuint 再自己截断

// ---------- 函数登记 ----------
// GL(name, ret, ...)：登记进 __DATA,swgl（名字 → 实现），eglGetProcAddress 与装载器（s2a_gl_lookup）都查这张表；跑在游戏栈上，只许做浅的事（改状态、录制）
// GLB(name, ret, ...)：同上，名字另记进 __DATA,swglb：交给游戏前由适配层（graphics/nvn/host_adapt.m）套 S2Arm 切栈桩，跑在宿主栈上（建对象、上传、提交、上屏：凡调 Metal 做重活的）
//   GLB 的函数体一律整个包在 @autoreleasepool 里（t_gl_api.sh 逐个核）：调用方是客体线程，没有会排空的自动释放池，Metal 的便利构造（命令缓冲、编码器、
//   渲染遍描述、各类描述 / 键串）若进了线程的隐式池就活到线程结束，命令缓冲连带它引用的每帧新贴图一起不放（11-01 街霸 30 周年合集每帧漏一张 384×224，低区耗尽崩溃）
#define GL(name, ret, ...)                                                                        \
    static ret gl_##name(__VA_ARGS__);                                                            \
    __attribute__((used, section("__DATA,swgl"))) static const hle_ent gle_##name = {#name, (void *)gl_##name}; \
    static ret gl_##name(__VA_ARGS__)
#define GLB(name, ret, ...)                                                                       \
    static ret gl_##name(__VA_ARGS__);                                                            \
    __attribute__((used, section("__DATA,swgl"))) static const hle_ent gle_##name = {#name, (void *)gl_##name}; \
    __attribute__((used, section("__DATA,swglb"))) static const char *const glb_##name = #name;   \
    static ret gl_##name(__VA_ARGS__)
#define GLOG(...) LOG("[gl] " __VA_ARGS__)
#define GLRED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[gl] 【红】 " __VA_ARGS__); } } while (0)   // 未实现的参数组合：报红写名字，每处只报一次

// ---------- 分片锚 ----------
// 各片的入口只登记在 __DATA,swgl 段里、没有对外被调用的符号：宿主把 libnvn.a 当静态库链接时，没被引用的 .o 整个不进来（10-25 数码宝贝：gl_buf.o 被丢，
//   glGenBuffers / glGenVertexArrays 等 13 个入口查表回空，游戏调空指针崩）。每片定义一个锚，gl_core.m 的 gl_lookup 引用全部锚；新加一片要在这里和 gl_core.m 各加一行
extern const char gl_slice_egl, gl_slice_gl_buf, gl_slice_gl_draw, gl_slice_gl_fb, gl_slice_gl_prog, gl_slice_gl_tex;

// ---------- 对象表（GL 名字 → 宿主对象；每类一张，0 号不用） ----------
typedef struct { uint32_t n, cap; void **v; } gl_names;
void *gl_name_get(gl_names *t, GLuint name);                 // 没有 → NULL
GLuint gl_name_new(gl_names *t, void *obj);                  // glGen* / glCreate*；obj == NULL：设 OOM、返回 0、不占槽
#define GL_NAME_DENSE_MAX (1u << 20)
int gl_name_put(gl_names *t, GLuint name, void *obj);         // glBind* 遇到没 Gen 过的名字时（兼容配置允许，core 不允许：报红后照建）
void *gl_name_del(gl_names *t, GLuint name);                 // glDelete*：返回对象，由调用方释放
void gl_names_clear(gl_names *t, void (*destroy)(void *));   // 仅在所有客体线程退出后调用

// ---------- 对象（绘制要读，结构定在这里；各片往里加字段先在 #272 提） ----------
struct gl_buffer {
    __strong id<MTLBuffer> buf, idx16; size_t size; uint64_t last_use;
    __strong NSMutableDictionary *fan32_cache;
    GLenum map_access; uint8_t used;   // map_access：glMapBuffer 的访问方式（0 = 没映射）；used：绑定过（glIsBuffer 规范：Gen 出的名字第一次绑定才成为缓冲对象）
    GLbitfield map_bits; size_t map_off, map_len;   // glMapBufferRange 的访问位与映射范围（glFlushMappedBufferRange 核范围用；glMapBuffer = 整块、位 0）
    GLbitfield storage;   // glBufferStorage 的标志位（不可变存储；0 = glBufferData 建的可变存储）
};   // idx16：整块 ubyte→ushort 副本；fan32_cache：本版内容按 (offset,count,type) 缓存的三角扇副本表。写入后失效；last_use 只跟踪原存储。
typedef struct { GLuint buf; GLint size; GLenum type; GLuint norm, enabled; GLsizei stride; GLintptr off; GLuint divisor; } gl_attrib;
struct gl_vao { gl_attrib a[16]; GLuint ebo; };                    // gl_buf.m
struct gl_rt;   // gl_prog.m：运行时兜底现编的一支（库里没有的源码，进程内常驻）
typedef struct { char key[17]; GLenum type; char *src; size_t len; int compiled, attached, deleted; struct gl_rt *rt; } gl_shader;   // gl_prog.m：key = 源码 sha256 前 16 位；attached：挂着它的程序数；deleted：glDeleteShader 时还挂着，等最后一个程序分离 / 删除才真删（DELETE_STATUS）；rt：库里没有、交运行时兜底的那一支（否则 NULL）
typedef struct { char name[64]; int off[2], count, stride; char type[8]; } gl_uniform;   // off[阶段]：在该阶段默认 uniform 块里的偏移（−1 = 该阶段没有）
struct gl_program {                                                // gl_prog.m
    GLuint sh[2]; int linked, validated;                           // validated：glValidateProgram 的结果（GL_VALIDATE_STATUS）
    __strong id<MTLFunction> fn[2];                                // 顶点 / 片元
    struct gl_rt *rt[2];                                           // 现编的阶段：函数在第一次画时取（gl_prog_fns），之前 fn 为 nil
    int ub_slot[2], ub_size[2]; uint8_t *ub[2];                    // 默认 uniform 块：Metal 缓冲槽、大小、CPU 影子（glUniform* 写，绘制时 setBytes）
    struct { char name[64]; int slot[2]; char type[32]; GLint unit; int arr, elem; } smp[16]; int nsmp; // #272：每阶段槽(-1=不用)、采样单元；采样器数组每个元素一项（arr = 数组长，非数组 1；elem = 元素号），同一数组的各项连续
    gl_uniform *u; int nu;                                          // uniform 位置 = 下标
    int writes_depth;                                               // 片元着色器写 gl_FragDepth（库表 D）：目标没深度时挂假深度（gl_draw.m）
    struct { char name[64]; int slot[2], size; GLuint binding; } blk[8]; int nblk;   // 命名统一块：块下标 = glGetUniformBlockIndex，每阶段 Metal 槽（-1 = 不用）、反射大小、glUniformBlockBinding 的绑定点（链接后为 0）
    struct { char name[64]; int slot, loc; char type[8]; } attr[16]; int nattr;   // 顶点输入：Metal 属性号 slot（库表 a:）、GL 属性号 loc（glGetAttribLocation 回它、绘制按它取数组）；type 是反射的 GLSL 类型名
    struct { char name[64]; GLuint idx; } bind[16]; int nbind;     // glBindAttribLocation 记下的「名字 → GL 属性号」，下一次链接生效（GL 语义）
    __strong NSMutableDictionary *pso;                              // gl_draw.m：按「顶点布局 + 目标格式 + 混合」取的管线
    char lkey[2][17]; char *lsrc[2];                                // 链接时两阶段着色器的源码键、现编阶段的源码副本（即时存档按它重建函数：着色器链接后常被删掉）
};
typedef struct gl_texture gl_texture;    // gl_tex.m：__strong id<MTLTexture> tex; 目标、格式、各级尺寸；首次使用时按已给的级建
typedef struct gl_sampler gl_sampler;    // gl_tex.m：__strong id<MTLSamplerState>
typedef struct gl_framebuffer gl_framebuffer;   // gl_fb.m：颜色附件 ×8 + 深度 / 模板；0 号 = 窗口表面
extern gl_names gl_buffers, gl_vaos, gl_shaders, gl_programs;   // gl_core.m 定义（各片共用的名字表）

// ---------- 上下文状态（一个上下文，绑在 MakeCurrent 的线程上） ----------
enum { GL_COLOR_TARGETS = 8 };
typedef struct { GLboolean mask[4], blend; GLenum eq[2], factor[4]; } gl_color_state;
typedef struct {
    EGLenum api;                                          // 创建上下文时选择的 OpenGL / OpenGL ES API
    GLuint vao, prog, fb_draw, fb_read, buf_bound[16];   // 当前绑定（buf_bound 按 gl_buf_slot() 的目标序号）
    GLint unpack_align, unpack_row, pack_align; // 上下文像素存储；eglCreateContext 初始4/0/4（pack_align：glGetTexImage 等读回的行对齐）。
    GLuint rb_bound; GLfloat poly_factor, poly_units, line_width, point_size;    // 当前渲染缓冲、多边形偏移（glPolygonOffset，开 GL_POLYGON_OFFSET_FILL 才生效）
    GLuint tex_unit[32][8], smp_unit[32], active_unit;    // 纹理单元 × 目标（gl_tex_slot()）、采样器
    gl_texture *default_tex[4];                           // 默认 2D / cube / 2D 数组 / 3D：上下文私有，同目标跨单元共用；不进名字表
    GLuint ubo[32]; GLintptr ubo_off[32]; GLsizeiptr ubo_size[32];
    GLint vp[4], sc[4]; GLfloat depth_range[2];           // 视口 / 裁剪（窗口坐标，左下原点）
    uint32_t enable;                                      // DEPTH_TEST / STENCIL_TEST / CULL_FACE / SCISSOR_TEST 等；BLEND 在 color[]
    gl_color_state color[GL_COLOR_TARGETS];                  // 按片元输出 / draw buffer 索引的混合与写掩码
    GLenum default_draw[GL_COLOR_TARGETS];                   // 默认 FBO 路由；命名 FBO 的路由由对象保存
    GLenum default_read;                                     // 默认 FBO 的读缓冲（glReadBuffer，初值 GL_BACK）；命名 FBO 的读缓冲由对象保存
    GLenum poly_mode;                                        // glPolygonMode（初值 GL_FILL；Metal 只有填充 / 线框）
    GLfloat attr_cur[16][4];                                 // 当前通用顶点属性（glVertexAttrib*，初值 0,0,0,1）：数组关掉的属性读它
    GLenum depth_func, cull, front, stencil_func[2], stencil_op[2][3];
    GLint stencil_ref[2]; GLuint stencil_mask[2], stencil_wmask[2]; GLboolean depth_mask; GLfloat blend_color[4], clear_color[4], clear_depth; GLint clear_stencil;
    GLenum error;                                         // glGetError：第一个没取走的错误码
    struct gl_vao default_vao;                            // 0 号顶点数组（GLES 缺省顶点数组）：每个上下文一个、不共享（GLES 3.2 §10.3.1 容器对象）
    uint32_t dirty;                                       // GLD_*：哪些状态变了、下一笔绘制要重取管线 / 重绑
} gl_state;
enum { GLE_CULL = 1, GLE_DEPTH = 2, GLE_STENCIL = 4, GLE_SCISSOR = 16, GLE_POLY_OFFSET = 32, GLE_COLOR_TABLE = 64, GLE_FB_SRGB = 128, GLE_DEBUG = 256, GLE_DEBUG_SYNC = 512 };   // gl_state.enable 的位；混合开关在 color[i].blend
enum { GLD_PIPE = 1, GLD_DSS = 2, GLD_VP = 4, GLD_SC = 8, GLD_RES = 16, GLD_VB = 32 };
extern __thread gl_state *gl_cur;   // 本线程的当前上下文状态（egl.m 在 MakeCurrent 时设；每个线程各绑各的，同 EGL 规范）
void gl_set_error(GLenum e);      // 只记第一个（规范：多个错误时 glGetError 逐个取，这里只留第一个，够用到普查出现反例为止）
void gl_set_error_at(GLenum e, const char *fn);   // 同上，另记是哪个函数置的：错误槽由空变非空时打一行（前 32 条）。游戏常在 glGetError 见错即中止（FINAL FANTASY VII），据此定位
#define gl_set_error(e) gl_set_error_at((e), __func__)

// ---------- 默认帧缓冲（窗口表面，egl.m） ----------
id<MTLTexture> gl_surface_color(void);   // 当前绘制表面这一帧要画进的颜色贴图（没有表面 → nil）
id<MTLTexture> gl_surface_depth(void);
id<MTLTexture> gl_surface_front(void);   // 上一次交换交出去的颜色贴图（glReadBuffer(GL_FRONT)；还没交换过 → nil）
void gl_surface_game(uint32_t *w, uint32_t *h);   // 游戏看到的表面宽高（宽屏时颜色贴图更宽：拷贝 / 读回按它换算横坐标）
void gl_surface_extent(GLint x1, GLint y1);   // 0 号帧缓冲本帧画到的右上角（视口 / 清除范围），上屏按它裁切（游戏按自己的 vi 层尺寸画）
void gl_surface_wid(uint32_t *color, uint32_t *depth);   // 当前绘制表面颜色 / 深度贴图的宽屏核心贴图号（宽屏关 = 0）

// ---------- 绘制目标（gl_fb.m）：当前绘制帧缓冲解析成 Metal 附件；0 号 = 窗口表面（深度模板同一张） ----------
// 颜色槽按片元输出路由排序；快照强持有附件，FBO 重绑 / 纹理重定义不改变已挂起 clear 的目标。
typedef struct { __strong id<MTLTexture> col[GL_COLOR_TARGETS], dep, sten; NSUInteger w, h; GLuint fb; uint32_t cid[GL_COLOR_TARGETS], did; int fake_dep; } gl_target;   // fake_dep：dep 是本层挂的假深度（GL 目标没有深度、程序写 gl_FragDepth），深度测试恒过、不写
int gl_draw_target(gl_target *t);        // 0 = 无目标 / 不完整 / 不支持（已报红）；附件纹理按本次提交登记使用
void gl_dev_init(void);                  // gl_draw.m：取共用设备、建本层队列与共享事件（一次）
void *gl_lookup(const char *name);       // gl_core.m：函数表查找（eglGetProcAddress、装载器），只回真实现
int gl_core_alias(const char *name, char *core, size_t cap);   // gl_core.m：扩展别名（ARB / EXT / OES / KHR）去后缀得核心名，语义不同的除外；是核心名回 1
void *gl_entry(const char *name);        // gl_core.m：交给游戏的入口 = 查表 + 重活（swglb）套 S2Arm 切栈桩；只给真实现（装载器 host_adapt.m s2a_gl_entry）
void *gl_stub(const char *name);         // gl_core.m：没实现的核心名的安全空实现（非核心名回空）；调用方找遍真实现之后才要
void *gl_entry_or_stub(const char *name); // gl_core.m：eglGetProcAddress 交出的 = gl_entry，没有时 gl_stub
int gl_is_big(const char *name);         // gl_core.m：要跑在宿主栈上的名字

// ---------- 编码（gl_draw.m 实现，各片调用） ----------
// 一帧一个 MTLCommandBuffer（前端自己的队列）；渲染编码器按「当前帧缓冲 + 附件加载动作」开，帧缓冲 / 附件变了或要做拷贝时收
id<MTLCommandBuffer> gl_cmd(void);                       // 本帧命令缓冲（没有就开）
id<MTLRenderCommandEncoder> gl_render_enc(void);         // 按当前帧缓冲开（或沿用）渲染编码器
id<MTLBlitCommandEncoder> gl_blit_enc(void);             // 兑现挂起清除、收掉渲染编码器，开拷贝编码器
void gl_end_enc(void);
uint64_t gl_commit(void);                                 // 提交本帧（eglSwapBuffers、glFinish、glFlush 时），返回 g_ev 的值
void gl_clear(GLbitfield mask);                           // 按当前清除值清当前帧缓冲（收掉编码器，下一遍以清除开）
void gl_wait(uint64_t v);                                 // 等本层共享事件到 v（glFinish、上屏前取回读）
struct gl_vao *gl_vao_cur(void);                        // gl_buf.m：当前顶点数组；0 号是 GLES 的缺省顶点数组（不在名字表里）
void gl_buffer_ref(struct gl_buffer *b);                   // 绑定当前存储进编码器时登记本次提交
GLuint gl_pack_buffer_name(void); struct gl_buffer *gl_pack_buffer(void);   // gl_buf.m：当前绑的像素打包缓冲（名字 / 对象，没绑 0 / NULL）
GLuint gl_unpack_buffer_name(void); struct gl_buffer *gl_unpack_buffer(void);   // gl_buf.m：当前绑的像素解包缓冲（名字 / 对象，没绑 0 / NULL）
int gl_buffer_busy(const struct gl_buffer *b);             // 当前存储仍被未完成的提交引用
extern id<MTLDevice> gl_dev; extern id<MTLSharedEvent> gl_ev;

// #272 纹理片导出：目标序号、逐笔绑定、指定级附件视图；未知目标回-1，无存储回nil。
int gl_tex_slot(GLenum target);                          // 2D 0 / 立方体 1 / 2D 数组 2 / 3D 3
int gl_tex_sampler_slot(const char *glsl_type);          // 采样器 GLSL 类型 → 同上的目标序号（阴影同非阴影；不认 = -1）
int gl_tex_prepare(const struct gl_program *p);         // 本笔开渲染编码器前，GPU 组好采样需要的 mip / cube
void gl_tex_context_drop(gl_state *st);                  // 释放默认纹理；宿主栈，上下文最终释放前调用
int gl_tex_unit_dims(GLuint unit, int slot, uint32_t *w, uint32_t *h); // 当前绑定的实际图像尺寸，包含默认纹理；不创建对象
int gl_tex_bind(id<MTLRenderCommandEncoder> re, const struct gl_program *p);
id<MTLTexture> gl_tex_mtl(GLuint name, GLint level);
id<MTLTexture> gl_tex_attach(GLuint name, GLenum target, GLint level);   // 帧缓冲附件：2D 或立方体某面的该级（渲染目标用途）；登记 GPU 写入与本次提交；没有 → nil
id<MTLTexture> gl_tex_export(GLuint name, GLenum *internal, uint32_t *levels);   // NVN 互操作：二维纹理的存储（不可变 = 整条级链）、内部格式、级数；没有 → nil
GLenum gl_tex_level_format(GLuint name, GLenum target, GLint level);   // 纹理某级（某面）的内部格式（没有 = 0；帧缓冲附件查询用）
uint64_t gl_seq_ref(void); // 当前命令缓冲将使用的提交序号。
int gl_seq_busy(uint64_t seq); // 该提交尚未完成。
// 跨队列同步（hle_nvn.c 的 nvnSyncCreateGLSync / nvnSyncInitializeFromFencedGLSync 用，gl_draw.m 实现）
uint64_t gl_sync_from_event(void *ev, uint64_t val);                             // 别的队列的共享事件 → GLsync 句柄（0 = 失败）
int gl_sync_event(uint64_t h, void **ev, uint64_t *val, int *committed);         // GLsync → 要等的共享事件与值；句柄无效回 0

// ---------- 宽屏第三层（gl_widen.inc，gl_draw.m 包含；接口 graphics/widen/export/s2a_widen.h；缺省开、S2A_WIDEN_DIRECT=0 关，关时全部原样） ----------
int gl_wd_on(void);
uint32_t gl_wd_new_id(void);
uint32_t gl_wd_rt(uint32_t id, uint32_t w, uint32_t h, uint32_t *th);                        // 建窗口表面：核心给的宽（不放宽 = w），*th = 核心给的高（不加高 = h）
// 附件影子挂在游戏的纹理 / 渲染缓冲对象上（名字会复用，不按名字查）：tex 影子（没放宽 = orig）、orig 建影子时的原图、id 核心贴图号（0 = 没有）、cpu 原图上传过内容
typedef struct { __strong id<MTLTexture> tex, orig; uint32_t id; int cpu; } gl_wd_shadow;
id<MTLTexture> gl_wd_att(gl_wd_shadow *s, id<MTLTexture> t, uint32_t *oid);                  // 帧缓冲附件：放宽的影子贴图，没放宽回原图；s = NULL：这种挂法不放宽
id<MTLTexture> gl_wd_sample(gl_wd_shadow *s, id<MTLTexture> level0, id<MTLTexture> image);   // 采样：当过附件且放宽了的纹理换成影子（level0 = 第 0 级存储）
void gl_wd_drop(gl_wd_shadow *s, int lost);                                                  // 影子作废（重定义 / 上传 / 删；游戏栈上只挂起，开渲染遍时释放）；lost：画进去的内容因此丢
gl_wd_shadow *gl_tex_wd(GLuint name);
int gl_tex_dims(GLuint name, uint32_t *w, uint32_t *h);                                       // gl_tex.m：纹理当前采样起始级的宽高（没有 = 0）                                                        // gl_tex.m：纹理的附件影子（没有这个纹理 = NULL）
void gl_wd_present(uint32_t id, uint32_t w, uint32_t h, int crop[4]);                        // 上屏裁剪按放宽换算

// ---------- 着色器库（gl_prog.m 实现；库表格式见 translator/gl_shaders.py 文件头） ----------
#include "gl_lib.h"

// ---------- 对公共部分的导出（host_adapt.m / 直连起手 / 画面检查用；docs/研究/GL前端接口.md 第 3 节） ----------
void dm_gl_stats(uint64_t *frames, uint64_t *draws, uint64_t *passes);
void dm_gl_bad_draws(uint64_t *feat, uint64_t *err);
void dm_gl_drain(void);
int dm_gl_request_stop(void);
int dm_gl_shutdown_session(void);
int gl_session_stopping(void);
void gl_egl_session_reset(void);
void gl_buf_session_reset(void);
void gl_tex_session_reset(void);
void gl_prog_session_reset(void);
int gl_prog_fns(struct gl_program *p);   // gl_prog.m：绘制前补齐现编阶段的函数（等那一支的 Metal 编译）；坏了回 0
void gl_fb_session_reset(void);
void gl_wd_release_id(uint32_t id);
extern _Atomic uint64_t gl_n_frames, gl_n_draws, gl_n_passes, gl_bad_feat, gl_bad_err;
extern _Atomic uint64_t gl_wait_n, gl_wait_ns;   // 游戏线程等 GPU 的次数与累计纳秒（gl_wait；egl.m 每 300 帧打一行拆账）

// ---------- 即时存档（苹果端缺省开、S2A_SNAP_MODE=0 关，见 runtime/src/snapsw.c；egl.m gl_snap_save / gl_snap_restore 按片调） ----------
// 格式按 runtime/src/snapfmt.h 的字段号记录（新版 App 读旧快照：缺的字段取缺省、不认的跳过），Metal 贴图 / 缓冲内容跟在各片的段尾后面整块存。
//   存在冻结之后（游戏线程全停在门里）、本层排空之后；取在新进程起好服务层、游戏线程还没起的时候（gl_snap_thread_bind 在各游戏线程就位时接回当前上下文）
#include "../../../../runtime/src/snapfmt.h"
#define GLSF_W(w, id, field) sfw_blob((w), (uint16_t)(id), &(field), (uint32_t)sizeof(field))
#define GLSF_R(r, id, field) sfr_into((r), (uint16_t)(id), &(field), (uint32_t)sizeof(field))
// 带布局指纹的结构（或结构数组）整块存的字段：快照里是旧布局时按字段转（snapfmt.h sfr_into_conv；name = 登记指纹的名字，T = 元素类型）
#define GLSF_RC(r, id, field, name, T) sfr_into_conv((r), (uint16_t)(id), &(field), (uint32_t)sizeof(field), (name), (uint32_t)sizeof(T))
int gl_buf_snap_save(FILE *f); int gl_buf_snap_restore(FILE *f);
int gl_tex_snap_save(FILE *f); int gl_tex_snap_restore(FILE *f);
int gl_tex_snap_one_save(FILE *f, gl_texture *t);   // 一个纹理对象（上下文的默认纹理也用它；t 可为 NULL）
gl_texture *gl_tex_snap_one_load(FILE *f);
int gl_prog_snap_save(FILE *f); int gl_prog_snap_restore(FILE *f);
int gl_fb_snap_save(FILE *f); int gl_fb_snap_restore(FILE *f);
int gl_draw_snap_save(FILE *f); int gl_draw_snap_restore(FILE *f);
int gl_snap_drain(void);
typedef snap_fp_cb gl_fp_cb;   // 按整块存的小结构的布局指纹 + 完整布局（snapfmt.h SNAP_FP_*；快照引擎恢复前逐个比）
void gl_tex_snap_fps(gl_fp_cb cb); void gl_fb_snap_fps(gl_fp_cb cb);
void gl_buf_snap_handout(void); uint64_t gl_buf_snap_map(const char *id);   // gl_buf.m：映射着的缓冲指针按身份登记 / 恢复时按身份建存储回新地址                                  // gl_draw.m：提交本层在途命令、等 GPU 与完成回调都结束
const char *gl_snap_str(const char *id);                  // gl_fb.m：glGetString / glGetStringi 交出的串按身份找回本进程的（"<名>:<es>" / "i:<号>"）
int nvn_snap_tex_save(FILE *f, void *tex) __attribute__((weak)); void *nvn_snap_tex_load(FILE *f) __attribute__((weak));   // 直连层 nvn_snap.inc：整张贴图连描述存 / 取（取回的已 retain，用 __bridge_transfer 接）；GL 单测只链本层，没有时快照存取判失败
