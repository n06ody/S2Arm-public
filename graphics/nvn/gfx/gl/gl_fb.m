// 状态与帧缓冲（分片「状态与帧缓冲」，#272）：Viewport / Scissor、Clear*、Enable / Disable、单源混合、深度比较 / 写入 / 范围、模板、多边形偏移、
// 帧缓冲 0（窗口表面）与帧缓冲对象（8 路颜色 + 深度 + 模板附件：纹理某级 / 渲染缓冲）、状态查询（glGet*）。绘制按 gl_state 取管线（gl_draw.m）
#include "gl_int.h"
const char gl_slice_gl_fb = 1;   // 分片锚（gl_core.m 引用：静态链接时整片进来，见 gl_int.h「分片锚」）
#include <math.h>

enum { GL_DEPTH_BUFFER_BIT = 0x100, GL_STENCIL_BUFFER_BIT = 0x400, GL_COLOR_BUFFER_BIT = 0x4000, GL_FRAMEBUFFER = 0x8D40,
       GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11,
       GL_POLYGON_OFFSET_FILL = 0x8037, GL_COLOR_TABLE = 0x80D0, GL_TEXTURE_2D = 0x0DE1, GL_DITHER = 0x0BD0, GL_MULTISAMPLE = 0x809D, GL_PROGRAM_POINT_SIZE = 0x8642,
       GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F, GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_STENCIL_ATTACHMENT = 0x8D20,
       GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x8CD6, GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7,
       GL_NONE = 0, GL_BACK = 0x0405, GL_BACK_LEFT = 0x0402,
       GL_FRAMEBUFFER_UNSUPPORTED = 0x8CDD,
       GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502, GL_OUT_OF_MEMORY = 0x0505, GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506 };
#define CTX(ret) do { if (!gl_cur) { GLRED1("%s：没有当前上下文", __func__); return ret; } } while (0)

GL(glViewport, void, GLint x, GLint y, GLsizei w, GLsizei h) {
    CTX(); if (w < 0 || h < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_cur->vp[0] = x; gl_cur->vp[1] = y; gl_cur->vp[2] = w; gl_cur->vp[3] = h; gl_cur->dirty |= GLD_VP;
}
GL(glScissor, void, GLint x, GLint y, GLsizei w, GLsizei h) {
    CTX(); if (w < 0 || h < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_cur->sc[0] = x; gl_cur->sc[1] = y; gl_cur->sc[2] = w; gl_cur->sc[3] = h; gl_cur->dirty |= GLD_SC;
}
static uint32_t cap_bit(GLenum cap) {
    switch (cap) { case GL_CULL_FACE: return GLE_CULL; case GL_DEPTH_TEST: return GLE_DEPTH; case GL_STENCIL_TEST: return GLE_STENCIL; case GL_SCISSOR_TEST: return GLE_SCISSOR;
                   case GL_POLYGON_OFFSET_FILL: return GLE_POLY_OFFSET; case GL_COLOR_TABLE: return GLE_COLOR_TABLE;
                   case 0x8DB9 /* GL_FRAMEBUFFER_SRGB */: return GLE_FB_SRGB;
                   case 0x92E0 /* GL_DEBUG_OUTPUT */: return GLE_DEBUG; case 0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */: return GLE_DEBUG_SYNC; }   // 只记开关：本层不发调试消息
                   //   （glDebugMessageCallback 是安全空实现）；小丑牌 / LÖVE 起局 glDisable(GL_DEBUG_OUTPUT)，原先置 0x500、LÖVE 查到 GL 错误抛异常   // 只记开关（LÖVE 11 起局先 glIsEnabled 再 glDisable，原先两处都置 0x500）
    return 0;
}
// 对 Metal 无作用的开关：GL_TEXTURE_2D（固定管线纹理，程序取代）、抖动、多重采样（单采样附件上无效）、程序点大小（着色器总写点大小）、立方体无缝（Metal 恒无缝）
static int cap_noop(GLenum cap) { return cap == GL_TEXTURE_2D || cap == GL_DITHER || cap == GL_MULTISAMPLE || cap == GL_PROGRAM_POINT_SIZE || cap == GL_TEXTURE_CUBE_MAP_SEAMLESS; }
static void cap_set(GLenum cap, int on) {
    CTX();
    if (cap == GL_BLEND) { for (int i = 0; i < GL_COLOR_TARGETS; i++) gl_cur->color[i].blend = on; gl_cur->dirty |= GLD_PIPE; return; }
    uint32_t b = cap_bit(cap);
    if (!b) { if (!cap_noop(cap)) { GLRED1("gl%s %#x 没实现", on ? "Enable" : "Disable", cap); gl_set_error(GL_INVALID_ENUM); } return; }
    if (b == GLE_FB_SRGB && on) { static int n; if (!n++) GLOG("GL_FRAMEBUFFER_SRGB 打开：Metal 的 sRGB 目标恒做线性 → sRGB 转换，关着时 GL 不转、这里照转（非 sRGB 目标两边一样）"); }
    if (on) gl_cur->enable |= b; else gl_cur->enable &= ~b;
    gl_cur->dirty |= GLD_PIPE | GLD_DSS | GLD_SC;
}
GL(glEnable, void, GLenum cap) { cap_set(cap, 1); }
GL(glDisable, void, GLenum cap) { cap_set(cap, 0); }
GL(glIsEnabled, GLboolean, GLenum cap) {
    CTX(0); if (cap == GL_BLEND) return gl_cur->color[0].blend;
    uint32_t b = cap_bit(cap); if (b) return (gl_cur->enable & b) != 0;
    if (cap_noop(cap)) return cap == GL_DITHER || cap == GL_MULTISAMPLE;
    gl_set_error(GL_INVALID_ENUM); return 0;
}
static int color_index(GLuint i) { if (i < GL_COLOR_TARGETS) return 1; gl_set_error(GL_INVALID_VALUE); return 0; }
static void cap_set_indexed(GLenum cap, GLuint i, int on) {
    CTX(); if (cap != GL_BLEND) { gl_set_error(GL_INVALID_ENUM); return; } if (!color_index(i)) return;
    gl_cur->color[i].blend = on; gl_cur->dirty |= GLD_PIPE;
}
GL(glEnablei, void, GLenum cap, GLuint i) { cap_set_indexed(cap, i, 1); }
GL(glDisablei, void, GLenum cap, GLuint i) { cap_set_indexed(cap, i, 0); }
GL(glIsEnabledi, GLboolean, GLenum cap, GLuint i) {
    CTX(0); if (cap != GL_BLEND) { gl_set_error(GL_INVALID_ENUM); return 0; } if (!color_index(i)) return 0;
    return gl_cur->color[i].blend;
}
// factor 按 srcRGB / dstRGB / srcAlpha / dstAlpha 排列；先核全组，错误调用不改变任何状态。
static int blend_factor(GLenum f) {
    if (f <= 1 || (f >= 0x0300 && f <= 0x0308) || (f >= 0x8001 && f <= 0x8004)) return 1;
    if (f == 0x8589 || (f >= 0x88F9 && f <= 0x88FB)) {
        GLRED1("glBlendFunc 双源混合因子 %#x 没实现", f); gl_set_error(GL_INVALID_ENUM); return 0;
    }
    gl_set_error(GL_INVALID_ENUM); return 0;
}
static void blend_func(int lo, int hi, GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    if (!blend_factor(sr) || !blend_factor(dr) || !blend_factor(sa) || !blend_factor(da)) return;
    for (int i = lo; i <= hi; i++) { gl_cur->color[i].factor[0] = sr; gl_cur->color[i].factor[1] = dr; gl_cur->color[i].factor[2] = sa; gl_cur->color[i].factor[3] = da; }
    gl_cur->dirty |= GLD_PIPE;
}
GL(glBlendFuncSeparate, void, GLenum sr, GLenum dr, GLenum sa, GLenum da) { CTX(); blend_func(0, GL_COLOR_TARGETS - 1, sr, dr, sa, da); }
GL(glBlendFunc, void, GLenum src, GLenum dst) { gl_glBlendFuncSeparate(src, dst, src, dst); }
GL(glBlendFuncSeparatei, void, GLuint i, GLenum sr, GLenum dr, GLenum sa, GLenum da) { CTX(); if (color_index(i)) blend_func(i, i, sr, dr, sa, da); }
GL(glBlendFunci, void, GLuint i, GLenum src, GLenum dst) { gl_glBlendFuncSeparatei(i, src, dst, src, dst); }
static int blend_equation(GLenum e) { return (e >= 0x8006 && e <= 0x8008) || e == 0x800A || e == 0x800B; }   // ADD / MIN / MAX / SUBTRACT / REVERSE_SUBTRACT
static void blend_eq(int lo, int hi, GLenum rgb, GLenum alpha) {
    if (!blend_equation(rgb) || !blend_equation(alpha)) { gl_set_error(GL_INVALID_ENUM); return; }
    for (int i = lo; i <= hi; i++) { gl_cur->color[i].eq[0] = rgb; gl_cur->color[i].eq[1] = alpha; } gl_cur->dirty |= GLD_PIPE;
}
GL(glBlendEquationSeparate, void, GLenum rgb, GLenum alpha) { CTX(); blend_eq(0, GL_COLOR_TARGETS - 1, rgb, alpha); }
GL(glBlendEquation, void, GLenum mode) { gl_glBlendEquationSeparate(mode, mode); }
GL(glBlendEquationSeparatei, void, GLuint i, GLenum rgb, GLenum alpha) { CTX(); if (color_index(i)) blend_eq(i, i, rgb, alpha); }
GL(glBlendEquationi, void, GLuint i, GLenum mode) { gl_glBlendEquationSeparatei(i, mode, mode); }
GL(glBlendColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    CTX(); const GLfloat color[] = {r, g, b, a};
    for (int i = 0; i < 4; i++) gl_cur->blend_color[i] = color[i] < 0 ? 0 : color[i] > 1 ? 1 : color[i];
}
static GLfloat depth_clamp(GLdouble v) { return (GLfloat)(v < 0 ? 0 : v > 1 ? 1 : v); }
GL(glDepthFunc, void, GLenum func) {
    CTX(); if (func < 0x0200 || func > 0x0207) { gl_set_error(GL_INVALID_ENUM); return; }   // NEVER..ALWAYS
    gl_cur->depth_func = func; gl_cur->dirty |= GLD_DSS;
}
GL(glDepthMask, void, GLuint flag) { CTX(); gl_cur->depth_mask = (GLboolean)flag != 0; gl_cur->dirty |= GLD_DSS; }
GL(glDepthRange, void, GLdouble near_val, GLdouble far_val) {
    CTX(); gl_cur->depth_range[0] = depth_clamp(near_val); gl_cur->depth_range[1] = depth_clamp(far_val); gl_cur->dirty |= GLD_VP;
}
GL(glDepthRangef, void, GLfloat near_val, GLfloat far_val) { gl_glDepthRange(near_val, far_val); }
GL(glFrontFace, void, GLenum mode) {
    CTX(); if (mode != 0x0900 && mode != 0x0901) { gl_set_error(GL_INVALID_ENUM); return; }   // CW / CCW
    gl_cur->front = mode; gl_cur->dirty |= GLD_PIPE;
}
GL(glCullFace, void, GLenum mode) {
    CTX(); if (mode != 0x0404 && mode != 0x0405 && mode != 0x0408) { gl_set_error(GL_INVALID_ENUM); return; }   // FRONT / BACK / FRONT_AND_BACK
    gl_cur->cull = mode; gl_cur->dirty |= GLD_PIPE;
}
// 模板：GL 的正面 / 背面各一组（[0] 正面、[1] 背面），非 Separate 的调用两面同设
static int stencil_op(GLenum o) { return o == 0 || o == 0x1E00 || o == 0x1E01 || o == 0x1E02 || o == 0x1E03 || o == 0x150A || o == 0x8507 || o == 0x8508; }   // ZERO KEEP REPLACE INCR DECR INVERT INCR_WRAP DECR_WRAP
static int faces(GLenum face, int *lo, int *hi) { *lo = face == 0x0405 ? 1 : 0; *hi = face == 0x0404 ? 0 : 1; return face == 0x0404 || face == 0x0405 || face == 0x0408; }
GL(glStencilFuncSeparate, void, GLenum face, GLenum func, GLint ref, GLuint mask) {
    CTX(); int lo, hi; if (!faces(face, &lo, &hi) || func < 0x0200 || func > 0x0207) { gl_set_error(GL_INVALID_ENUM); return; }
    for (int i = lo; i <= hi; i++) { gl_cur->stencil_func[i] = func; gl_cur->stencil_ref[i] = ref; gl_cur->stencil_mask[i] = mask; }
    gl_cur->dirty |= GLD_DSS;
}
GL(glStencilFunc, void, GLenum func, GLint ref, GLuint mask) { gl_glStencilFuncSeparate(0x0408, func, ref, mask); }
GL(glStencilOpSeparate, void, GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    CTX(); int lo, hi; if (!faces(face, &lo, &hi) || !stencil_op(sfail) || !stencil_op(dpfail) || !stencil_op(dppass)) { gl_set_error(GL_INVALID_ENUM); return; }
    for (int i = lo; i <= hi; i++) { gl_cur->stencil_op[i][0] = sfail; gl_cur->stencil_op[i][1] = dpfail; gl_cur->stencil_op[i][2] = dppass; }
    gl_cur->dirty |= GLD_DSS;
}
GL(glStencilOp, void, GLenum sfail, GLenum dpfail, GLenum dppass) { gl_glStencilOpSeparate(0x0408, sfail, dpfail, dppass); }
GL(glStencilMaskSeparate, void, GLenum face, GLuint mask) {
    CTX(); int lo, hi; if (!faces(face, &lo, &hi)) { gl_set_error(GL_INVALID_ENUM); return; }
    for (int i = lo; i <= hi; i++) gl_cur->stencil_wmask[i] = mask;
    gl_cur->dirty |= GLD_DSS;
}
GL(glStencilMask, void, GLuint mask) { gl_glStencilMaskSeparate(0x0408, mask); }
// 线宽：Metal 光栅化只有 1 像素宽的线；记下宽度供查询，画线照 1 像素（GL ES 允许实现的宽线范围只有 1）
GL(glLineWidth, void, GLfloat w) {
    CTX(); if (!(w > 0)) { gl_set_error(GL_INVALID_VALUE); return; }
    if (w != 1) GLRED1("glLineWidth(%g)：Metal 没有宽线，线照 1 像素宽画（宽度只记作查询值）", w);
    gl_cur->line_width = w;
}
// 点大小（桌面 GL）：记作上下文状态供查询；点的实际大小由顶点着色器的 gl_PointSize 决定（core 配置开 PROGRAM_POINT_SIZE 的情形）
GL(glPointSize, void, GLfloat s) { CTX(); if (!(s > 0)) { gl_set_error(GL_INVALID_VALUE); return; } gl_cur->point_size = s; }
// 实现提示：只核枚举，不改行为（规范允许实现忽略提示）
GL(glHint, void, GLenum target, GLenum mode) {
    CTX(); (void)target;
    if (mode != 0x1100 && mode != 0x1101 && mode != 0x1102) { gl_set_error(GL_INVALID_ENUM); return; }   // DONT_CARE / FASTEST / NICEST
}
// 多边形模式（桌面 GL，Slay the Spire）：GL_LINE → Metal 线框填充（setTriangleFillMode，gl_draw.m setup）；GL_POINT Metal 没有，报红后按填充画。
//   Metal 只能两面同设：只收 GL_FRONT_AND_BACK（core 配置的规定；兼容配置的单面设置报红）
GL(glPolygonMode, void, GLenum face, GLenum mode) {
    CTX(); if (mode != 0x1B00 && mode != 0x1B01 && mode != 0x1B02) { gl_set_error(GL_INVALID_ENUM); return; }   // POINT / LINE / FILL
    if (face != 0x0408) { if (face == 0x0404 || face == 0x0405) GLRED1("glPolygonMode 单面 %#x：Metal 只能正反面同设（按 core 报 GL_INVALID_ENUM）", face); gl_set_error(GL_INVALID_ENUM); return; }
    if (mode == 0x1B00) GLRED1("glPolygonMode(GL_POINT)：Metal 没有点多边形模式，按填充画");
    gl_cur->poly_mode = mode; gl_cur->dirty |= GLD_PIPE;
}
GL(glPolygonOffset, void, GLfloat factor, GLfloat units) { CTX(); gl_cur->poly_factor = factor; gl_cur->poly_units = units; }
GL(glClearColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { CTX(); gl_cur->clear_color[0] = r; gl_cur->clear_color[1] = g; gl_cur->clear_color[2] = b; gl_cur->clear_color[3] = a; }
GL(glClearDepth, void, GLdouble d) { CTX(); gl_cur->clear_depth = depth_clamp(d); }
GL(glClearDepthf, void, GLfloat d) { gl_glClearDepth(d); }
GL(glClearStencil, void, GLint s) { CTX(); gl_cur->clear_stencil = s; }
GL(glColorMask, void, GLuint r, GLuint g, GLuint b, GLuint a) {
    CTX(); for (int i = 0; i < GL_COLOR_TARGETS; i++) {
        gl_cur->color[i].mask[0] = (r & 0xff) != 0; gl_cur->color[i].mask[1] = (g & 0xff) != 0;
        gl_cur->color[i].mask[2] = (b & 0xff) != 0; gl_cur->color[i].mask[3] = (a & 0xff) != 0;
    } gl_cur->dirty |= GLD_PIPE;
}
GL(glColorMaski, void, GLuint i, GLuint r, GLuint g, GLuint b, GLuint a) {
    CTX(); if (!color_index(i)) return;
    gl_cur->color[i].mask[0] = (r & 0xff) != 0; gl_cur->color[i].mask[1] = (g & 0xff) != 0;
    gl_cur->color[i].mask[2] = (b & 0xff) != 0; gl_cur->color[i].mask[3] = (a & 0xff) != 0; gl_cur->dirty |= GLD_PIPE;
}
// 局部 / 部分通道清除会编码 Metal 绘制，须经宿主切栈入口。
GLB(glClear, void, GLbitfield mask) { @autoreleasepool {
    CTX(); if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!gl_cur->depth_mask) mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    int color = 0; for (int i = 0; i < GL_COLOR_TARGETS; i++) for (int c = 0; c < 4; c++) color |= gl_cur->color[i].mask[c];
    if (!color) mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    if (!(gl_cur->stencil_wmask[0] & 0xff)) mask &= ~(GLbitfield)GL_STENCIL_BUFFER_BIT;
    if (!mask) return;
    gl_clear(mask);
}}
// 按缓冲清除（GL 3.0 / ES 3.0 §17.4.3.1）：GL_COLOR 只清第 i 路 draw buffer，GL_DEPTH 只清深度；同 glClear 受写掩码和裁剪约束，不改上下文的清除值。
//   做法：临时只留第 i 路的颜色写掩码、换上这次的值，走 glClear 的同一条路，再还原（LÖVE 引擎在 GL 3.0 以上用它清画布）
GLB(glClearBufferfv, void, GLenum buffer, GLint i, const GLfloat *value) { @autoreleasepool {
    CTX();
    if (buffer == 0x1800) {   // GL_COLOR
        if (i < 0 || i >= GL_COLOR_TARGETS) { gl_set_error(GL_INVALID_VALUE); return; }
        GLuint mask[GL_COLOR_TARGETS][4]; GLfloat cc[4]; memcpy(cc, gl_cur->clear_color, sizeof cc); int any = 0;
        for (int k = 0; k < GL_COLOR_TARGETS; k++) for (int c = 0; c < 4; c++) { mask[k][c] = gl_cur->color[k].mask[c]; if (k != i) gl_cur->color[k].mask[c] = 0; else any |= mask[k][c]; }
        memcpy(gl_cur->clear_color, value, sizeof cc);
        if (any) gl_clear(GL_COLOR_BUFFER_BIT);
        for (int k = 0; k < GL_COLOR_TARGETS; k++) for (int c = 0; c < 4; c++) gl_cur->color[k].mask[c] = mask[k][c];
        memcpy(gl_cur->clear_color, cc, sizeof cc);
        return;
    }
    if (buffer == 0x1801) {   // GL_DEPTH：drawbuffer 必须是 0
        if (i != 0) { gl_set_error(GL_INVALID_VALUE); return; }
        if (!gl_cur->depth_mask) return;
        GLfloat d = gl_cur->clear_depth; gl_cur->clear_depth = depth_clamp(value[0]); gl_clear(GL_DEPTH_BUFFER_BIT); gl_cur->clear_depth = d;
        return;
    }
    gl_set_error(GL_INVALID_ENUM);
}}
// ---------- 帧缓冲对象与渲染缓冲 ----------
// 附件存「对象 + 级 / 面」不存 Metal 贴图：纹理重定义某级（TexImage2D 换存储）后，绘制时重新解析就画进新存储，同 GL 的附件语义
enum { ATT_COL = 0, ATT_DEPTH = GL_COLOR_TARGETS, ATT_STEN, ATT_N };
typedef struct { int kind; GLuint name; GLenum target; GLint level; } gl_att;   // kind：0 无 / 1 纹理 / 2 渲染缓冲
struct gl_framebuffer {
    gl_att att[ATT_N]; GLenum draw[GL_COLOR_TARGETS], read; int used;   // read：glReadBuffer（初值 COLOR_ATTACHMENT0）；used：绑定过（glIsFramebuffer）
    __strong id<MTLTexture> stencil_parent, stencil_view;   // 独立 depth + packed stencil：缓存实际存储的 X32 平面，避免每笔换 view / 切遍
};
typedef struct { __strong id<MTLTexture> tex; GLenum format; gl_wd_shadow wd; int used; } gl_renderbuffer;   // wd：宽屏附件影子（gl_widen.inc）；used：绑定过（glIsRenderbuffer）
static gl_names framebuffers, renderbuffers;
static void framebuffer_destroy(void *object) {
    struct gl_framebuffer *f = object; f->stencil_parent = f->stencil_view = nil; free(f);
}
static void renderbuffer_destroy(void *object) {
    gl_renderbuffer *r = object; gl_wd_drop(&r->wd, 0); r->tex = nil; free(r);
}
void gl_fb_session_reset(void) {
    gl_names_clear(&framebuffers, framebuffer_destroy); gl_names_clear(&renderbuffers, renderbuffer_destroy);
}
// 作废附件内容（GL 4.3 / ES 3.0 §17.4.4）：只是告诉实现这些附件的内容以后不再需要，实现可以不做任何事。只核参数（LÖVE 切画布时对深度 / 模板调它）
GL(glInvalidateFramebuffer, void, GLenum target, GLsizei n, const GLenum *attachments) {
    CTX(); (void)attachments;
    if (target != 0x8D40 && target != 0x8CA9 && target != 0x8CA8) { gl_set_error(GL_INVALID_ENUM); return; }   // FRAMEBUFFER / DRAW_ / READ_FRAMEBUFFER
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
}
GL(glGenFramebuffers, void, GLsizei n, GLuint *out) {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct gl_framebuffer *f = calloc(1, sizeof *f); if (f) f->draw[0] = f->read = GL_COLOR_ATTACHMENT0;
        out[i] = gl_name_new(&framebuffers, f);
    }
}
GL(glDeleteFramebuffers, void, GLsizei n, const GLuint *names) {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct gl_framebuffer *f = gl_name_del(&framebuffers, names[i]); if (!f) continue;
        if (gl_cur->fb_draw == names[i]) gl_cur->fb_draw = 0;   // 删掉绑着的帧缓冲 = 绑回 0 号（规范）
        if (gl_cur->fb_read == names[i]) gl_cur->fb_read = 0;
        f->stencil_view = f->stencil_parent = nil; free(f);
    }
}
GL(glBindFramebuffer, void, GLenum target, GLuint name) {
    CTX(); if (target != GL_FRAMEBUFFER && target != 0x8CA9 && target != 0x8CA8) { gl_set_error(GL_INVALID_ENUM); return; }   // GL_FRAMEBUFFER / DRAW / READ
    if (name && !gl_name_get(&framebuffers, name)) { gl_set_error(GL_INVALID_OPERATION); return; }   // core：只认 glGen 过的名字
    if (name) ((struct gl_framebuffer *)gl_name_get(&framebuffers, name))->used = 1;
    if (target != 0x8CA8) gl_cur->fb_draw = name;
    if (target != 0x8CA9) gl_cur->fb_read = name;
}
static GLenum *draw_routes(void) {
    if (!gl_cur->fb_draw) return gl_cur->default_draw;
    struct gl_framebuffer *f = gl_name_get(&framebuffers, gl_cur->fb_draw);
    if (!f) { gl_set_error(GL_INVALID_OPERATION); return NULL; } return f->draw;
}
static int color_attachment(GLenum b) { return b >= GL_COLOR_ATTACHMENT0 && b < GL_COLOR_ATTACHMENT0 + GL_COLOR_TARGETS; }
static int default_buffer(GLenum b) { return b >= 0x0400 && b <= 0x0408; }   // FRONT_LEFT .. FRONT_AND_BACK
static int draw_buffer_valid(GLenum b, int multiple, GLsizei n) {
    if (b == GL_NONE) return 1;
    if (multiple && gl_cur->api == 0x30A0) {
        // ES 3.2 表 15.5：先辨合法枚举，再核当前 FBO；BACK_LEFT 等桌面枚举不合法。
        if (b != GL_BACK && !color_attachment(b)) { gl_set_error(GL_INVALID_ENUM); return 0; }
        if (gl_cur->fb_draw ? b == GL_BACK : b != GL_BACK) { gl_set_error(GL_INVALID_OPERATION); return 0; }
        return 1;
    }
    if (gl_cur->fb_draw) {
        if (color_attachment(b)) return 1;
        // 桌面表 17.6 的单个窗口缓冲枚举有效，但不可用于命名 FBO。
        int unavailable = multiple ? b >= 0x0400 && b <= 0x0403 :
            default_buffer(b) || (b >= GL_COLOR_ATTACHMENT0 && b < GL_COLOR_ATTACHMENT0 + 32);
        gl_set_error(unavailable ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        return 0;
    }
    if (color_attachment(b)) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    if (b == GL_BACK) { if (multiple && n != 1) { gl_set_error(GL_INVALID_OPERATION); return 0; } return 1; }
    if (b == GL_BACK_LEFT) return 1;
    if (default_buffer(b)) { gl_set_error(multiple && (b == 0x0404 || b >= 0x0406) ? GL_INVALID_ENUM : GL_INVALID_OPERATION); return 0; }
    gl_set_error(GL_INVALID_ENUM); return 0;
}
GL(glDrawBuffers, void, GLsizei n, const GLenum *bufs) {
    CTX(); if (n < 0 || n > GL_COLOR_TARGETS) { gl_set_error(GL_INVALID_VALUE); return; }
    GLenum *route = draw_routes(); if (!route) return;
    int es = gl_cur->api == 0x30A0;   // EGL_OPENGL_ES_API：ES 只允许输出 i 对应附件 i；桌面 GL 可任意置换。
    if (es && !gl_cur->fb_draw && n != 1) { gl_set_error(GL_INVALID_OPERATION); return; }
    for (int i = 0; i < n; i++) {
        if (!draw_buffer_valid(bufs[i], 1, n)) return;
        if (es && bufs[i] != GL_NONE && (gl_cur->fb_draw ? bufs[i] != GL_COLOR_ATTACHMENT0 + (GLenum)i : bufs[i] != GL_BACK)) {
            gl_set_error(GL_INVALID_OPERATION); return;
        }
        for (int j = 0; j < i; j++) if (bufs[i] != GL_NONE && bufs[i] == bufs[j]) { gl_set_error(GL_INVALID_OPERATION); return; }
    }
    for (int i = 0; i < GL_COLOR_TARGETS; i++) route[i] = i < n ? bufs[i] : GL_NONE;
    gl_cur->dirty |= GLD_PIPE;
}
GL(glDrawBuffer, void, GLenum b) {
    CTX(); GLenum *route = draw_routes(); if (!route || !draw_buffer_valid(b, 0, 1)) return;
    route[0] = b; for (int i = 1; i < GL_COLOR_TARGETS; i++) route[i] = GL_NONE;
    gl_cur->dirty |= GLD_PIPE;
}
GL(glGenRenderbuffers, void, GLsizei n, GLuint *out) {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) out[i] = gl_name_new(&renderbuffers, calloc(1, sizeof(gl_renderbuffer)));
}
GL(glDeleteRenderbuffers, void, GLsizei n, const GLuint *names) {
    CTX(); if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        gl_renderbuffer *r = gl_name_del(&renderbuffers, names[i]); if (!r) continue;
        if (gl_cur->rb_bound == names[i]) gl_cur->rb_bound = 0;
        renderbuffer_destroy(r);   // 已编码的绘制持有旧贴图。
    }
}
GL(glBindRenderbuffer, void, GLenum target, GLuint name) {
    CTX(); if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }
    if (name && !gl_name_get(&renderbuffers, name)) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (name) ((gl_renderbuffer *)gl_name_get(&renderbuffers, name))->used = 1;
    gl_cur->rb_bound = name;
}
// glIs*：名字在表里、且绑定过（Gen 出、没绑过的名字还不是对象：GL 4.5 §9.2 / §9.2.4）
GL(glIsFramebuffer, GLboolean, GLuint name) { CTX(0); struct gl_framebuffer *f = gl_name_get(&framebuffers, name); return f && f->used; }
GL(glIsRenderbuffer, GLboolean, GLuint name) { CTX(0); gl_renderbuffer *r = gl_name_get(&renderbuffers, name); return r && r->used; }
// 渲染缓冲格式 → Metal：深度 24 位一律存 32 位浮点（Metal 在 Apple GPU 上没有 24 位深度），带模板的存 Depth32Float_Stencil8
static MTLPixelFormat rb_format(GLenum f) {
    switch (f) {
        case 0x8058: case 0x1908: return MTLPixelFormatRGBA8Unorm;            // RGBA8 / RGBA
        case 0x8051: case 0x1907: return MTLPixelFormatRGBA8Unorm;            // RGB8 / RGB：多出的 alpha 恒按 1 读（混合不读目标 alpha 时无差别）
        case 0x8C43: return MTLPixelFormatRGBA8Unorm_sRGB;                     // SRGB8_ALPHA8
        case 0x881A: return MTLPixelFormatRGBA16Float; case 0x8814: return MTLPixelFormatRGBA32Float;   // RGBA16F / RGBA32F
        case 0x822D: return MTLPixelFormatR16Float; case 0x822E: return MTLPixelFormatR32Float;         // R16F / R32F
        case 0x8C3A: return MTLPixelFormatRG11B10Float; case 0x8059: return MTLPixelFormatRGB10A2Unorm;  // R11F_G11F_B10F / RGB10_A2
        case 0x81A5: return MTLPixelFormatDepth16Unorm;                       // DEPTH_COMPONENT16
        case 0x1902: case 0x81A6: case 0x81A7: case 0x8CAC: return MTLPixelFormatDepth32Float;   // DEPTH_COMPONENT / 24 / 32 / 32F
        case 0x84F9: case 0x88F0: case 0x8CAD: return MTLPixelFormatDepth32Float_Stencil8;       // DEPTH_STENCIL / DEPTH24_STENCIL8 / DEPTH32F_STENCIL8
        case 0x8D48: return MTLPixelFormatStencil8;                           // STENCIL_INDEX8
    }
    return MTLPixelFormatInvalid;
}
// 渲染缓冲查询（FINAL FANTASY VII 建帧缓冲后读回宽高 / 格式）：宽、高、内部格式、采样数；其它参数报红
GL(glGetRenderbufferParameteriv, void, GLenum target, GLenum what, GLint *v) {
    CTX(); if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }
    gl_renderbuffer *r = gl_name_get(&renderbuffers, gl_cur->rb_bound); if (!r) { gl_set_error(GL_INVALID_OPERATION); return; }
    switch (what) {
        case 0x8D42: *v = r->tex ? (GLint)r->tex.width : 0; return;              // RENDERBUFFER_WIDTH
        case 0x8D43: *v = r->tex ? (GLint)r->tex.height : 0; return;             // RENDERBUFFER_HEIGHT
        case 0x8D44: *v = r->format ? (GLint)r->format : 0x8056; return;          // RENDERBUFFER_INTERNAL_FORMAT（没建存储 = RGBA4，GL 缺省）
        case 0x8CAB: *v = r->tex ? (GLint)(r->tex.sampleCount > 1 ? r->tex.sampleCount : 0) : 0; return;   // RENDERBUFFER_SAMPLES
    }
    GLRED1("glGetRenderbufferParameteriv 参数 %#x 没实现", what); gl_set_error(GL_INVALID_ENUM);
}
// 渲染缓冲存储；samples > 0 = 多重采样（glRenderbufferStorageMultisample）：取设备支持的、不少于所求的最小样本数（GL：实际样本数 ≥ 所求，且至少 2），
//   存成 Metal 2D 多重采样贴图；绘制时管线光栅样本数随附件（gl_draw.m），解析走 glBlitFramebuffer（blit_color 的多重采样 → 单采样分支）
static void rb_storage(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h) {
    if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }
    gl_renderbuffer *r = gl_name_get(&renderbuffers, gl_cur->rb_bound); if (!r) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (w < 0 || h < 0 || w > 16384 || h > 16384 || samples < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (samples > 8) { gl_set_error(gl_cur->api == 0x30A0 ? GL_INVALID_OPERATION : GL_INVALID_VALUE); return; }   // > MAX_SAMPLES：ES 3.2 §9.2.4 报 INVALID_OPERATION、GL 4.5 报 INVALID_VALUE
    MTLPixelFormat pf = rb_format(internal);
    if (pf == MTLPixelFormatInvalid) { GLRED1("glRenderbufferStorage 内部格式 %#x 没实现", internal); gl_set_error(GL_INVALID_ENUM); gl_bad_feat++; return; }
    gl_dev_init();
    NSUInteger n = 1;
    if (samples > 0) { for (n = samples < 2 ? 2 : (NSUInteger)samples; n <= 8 && ![gl_dev supportsTextureSampleCount:n]; n++) ;
        if (n > 8) { GLRED1("设备不支持 %d 及以上的多重采样", samples); gl_set_error(GL_INVALID_OPERATION); gl_bad_feat++; return; } }
    @autoreleasepool {
        r->tex = nil; r->format = internal; if (!w || !h) return;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        if (n > 1) { d.textureType = MTLTextureType2DMultisample; d.sampleCount = n; }
        d.usage = MTLTextureUsageRenderTarget;
        if (pf == MTLPixelFormatDepth32Float_Stencil8) d.usage |= MTLTextureUsagePixelFormatView;
        d.storageMode = MTLStorageModePrivate;
        r->tex = [gl_dev newTextureWithDescriptor:d];
        if (!r->tex) { GLOG("【红】 渲染缓冲 %dx%d 格式 %#x（%lu 样本）分配失败", w, h, internal, (unsigned long)n); gl_set_error(GL_OUT_OF_MEMORY); }
    }
}
GLB(glRenderbufferStorage, void, GLenum target, GLenum internal, GLsizei w, GLsizei h) { @autoreleasepool { CTX(); rb_storage(target, 0, internal, w, h); }}
GLB(glRenderbufferStorageMultisample, void, GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h) { @autoreleasepool { CTX(); rb_storage(target, samples, internal, w, h); }}
static struct gl_framebuffer *draw_fb(GLenum target) {
    if (target != GL_FRAMEBUFFER && target != 0x8CA9 && target != 0x8CA8) { gl_set_error(GL_INVALID_ENUM); return NULL; }
    struct gl_framebuffer *f = gl_name_get(&framebuffers, target == 0x8CA8 ? gl_cur->fb_read : gl_cur->fb_draw);
    if (!f) gl_set_error(GL_INVALID_OPERATION);   // 0 号的附件不可改
    return f;
}
static void attach(struct gl_framebuffer *f, GLenum attachment, gl_att a) {
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT)
        f->stencil_view = f->stencil_parent = nil;
    if (color_attachment(attachment)) f->att[attachment - GL_COLOR_ATTACHMENT0] = a;
    else if (attachment == GL_DEPTH_ATTACHMENT) f->att[ATT_DEPTH] = a;
    else if (attachment == GL_STENCIL_ATTACHMENT) f->att[ATT_STEN] = a;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->att[ATT_DEPTH] = f->att[ATT_STEN] = a;
    else gl_set_error(GL_INVALID_ENUM);
}
GL(glFramebufferTexture2D, void, GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    CTX(); struct gl_framebuffer *f = draw_fb(target); if (!f) return;
    if (tex && (textarget != 0x0DE1 && (textarget < 0x8515 || textarget > 0x851A))) { gl_set_error(GL_INVALID_OPERATION); return; }   // TEXTURE_2D / CUBE_MAP_POSITIVE_X..NEGATIVE_Z
    if (level < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    attach(f, attachment, (gl_att){tex ? 1 : 0, tex, textarget, level});
}
GL(glFramebufferRenderbuffer, void, GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    CTX(); struct gl_framebuffer *f = draw_fb(target); if (!f) return;
    if (rb && (rbtarget != GL_RENDERBUFFER || !gl_name_get(&renderbuffers, rb))) { gl_set_error(rbtarget != GL_RENDERBUFFER ? GL_INVALID_ENUM : GL_INVALID_OPERATION); return; }
    attach(f, attachment, (gl_att){rb ? 2 : 0, rb, GL_RENDERBUFFER, 0});
}
static id<MTLTexture> att_tex(const gl_att *a) {
    if (a->kind == 1) return gl_tex_attach(a->name, a->target, a->level);
    if (a->kind == 2) { gl_renderbuffer *r = gl_name_get(&renderbuffers, a->name); return r ? r->tex : nil; }
    return nil;
}
// 附件对象的宽屏影子：只放宽 2D 纹理第 0 级与渲染缓冲（别的挂法 NULL = 不放宽）
static gl_wd_shadow *att_wd(const gl_att *a) {
    if (a->kind == 1) return a->level || a->target != 0x0DE1 ? NULL : gl_tex_wd(a->name);
    if (a->kind == 2) { gl_renderbuffer *r = gl_name_get(&renderbuffers, a->name); return r ? &r->wd : NULL; }
    return NULL;
}
static int has_depth(MTLPixelFormat f) { return f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8; }
static int has_stencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8; }
// 同一规则供查询与实际绘制使用：不完整 / Metal 不支持的组合不得静默丢掉某个 GL 平面。
static GLenum fb_status(struct gl_framebuffer *f, __strong id<MTLTexture> images[ATT_N]) {
    int any = 0;
    for (int i = 0; i < ATT_N; i++) if (f->att[i].kind) {
        if (!images[i] || !(images[i].usage & MTLTextureUsageRenderTarget)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if ((i < GL_COLOR_TARGETS && (has_depth(images[i].pixelFormat) || has_stencil(images[i].pixelFormat))) ||
            (i == ATT_DEPTH && !has_depth(images[i].pixelFormat)) || (i == ATT_STEN && !has_stencil(images[i].pixelFormat))) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        any = 1;
    }
    if (!any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    NSUInteger samples = 0;   // 各附件样本数必须一致（GL：FRAMEBUFFER_INCOMPLETE_MULTISAMPLE；Metal 同样要求）
    for (int i = 0; i < ATT_N; i++) if (images[i]) { if (!samples) samples = images[i].sampleCount; else if (images[i].sampleCount != samples) return 0x8D56; }
    id<MTLTexture> d = images[ATT_DEPTH], s = images[ATT_STEN];
    if (d && s && d != s) {
        if (d.width != s.width || d.height != s.height) return GL_FRAMEBUFFER_UNSUPPORTED;
        // packed depth 不能与别张 stencil 配对，且 packed 无法创建纯 Depth32Float view。
        // GL 4.5 §9.4.3 只强制同一 depth/stencil image；其他组合可以明确返回 UNSUPPORTED。
        if (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || ![gl_dev supportsFamily:MTLGPUFamilyApple2]) return GL_FRAMEBUFFER_UNSUPPORTED;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && !(s.usage & MTLTextureUsagePixelFormatView)) return GL_FRAMEBUFFER_UNSUPPORTED;
    }
    return GL_FRAMEBUFFER_COMPLETE;
}
GL(glCheckFramebufferStatus, GLenum, GLenum target) {
    CTX(0); if (target != GL_FRAMEBUFFER && target != 0x8CA9 && target != 0x8CA8) { gl_set_error(GL_INVALID_ENUM); return 0; }
    GLuint name = target == 0x8CA8 ? gl_cur->fb_read : gl_cur->fb_draw;
    if (!name) return gl_surface_color() ? GL_FRAMEBUFFER_COMPLETE : 0x8219;   // 0 号：无表面上下文没有缺省帧缓冲 → GL_FRAMEBUFFER_UNDEFINED（GLES 3.2 §9.4.2、EGL_KHR_surfaceless_context）
    struct gl_framebuffer *f = gl_name_get(&framebuffers, name); if (!f) return GL_FRAMEBUFFER_COMPLETE;
    id<MTLTexture> images[ATT_N] = {nil};
    for (int i = 0; i < ATT_N; i++) images[i] = att_tex(&f->att[i]);
    return fb_status(f, images);
}
// Metal 不允许同一颜色贴图在一遍占多槽；按片元输出槽顺序保留第一路。
static void unique_colors(gl_target *t) {
    for (int i = 0; i < GL_COLOR_TARGETS; i++) if (t->col[i]) {
        for (int j = 0; j < i; j++) if (t->col[i] == t->col[j]) {
            GLRED1("重复颜色槽 %d 与 %d 使用同一 Metal 贴图：保留槽 %d，槽 %d 置空", j, i, j, i);
            t->col[i] = nil; t->cid[i] = 0; break;
        }
    }
}
int gl_draw_target(gl_target *t) {
    *t = (gl_target){0}; t->fb = gl_cur->fb_draw;
    if (!t->fb) {
        id<MTLTexture> c = gl_surface_color(); t->dep = t->sten = gl_surface_depth();
        // 无表面上下文画 0 号帧缓冲：缺省帧缓冲不存在 = 不完整，绘制 / 清除报 INVALID_FRAMEBUFFER_OPERATION（GLES 3.2 §9.4.4、EGL_KHR_surfaceless_context）
        if (!c) { GLRED1("没有绘制表面（本线程的当前上下文没绑表面，0 号帧缓冲不存在）"); gl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return 0; }
        uint32_t cid; gl_surface_wid(&cid, &t->did);
        for (int i = 0; i < GL_COLOR_TARGETS; i++) if (gl_cur->default_draw[i] != GL_NONE) { t->col[i] = c; t->cid[i] = cid; }
        t->w = c.width; t->h = c.height;
        if (t->dep) { t->w = MIN(t->w, t->dep.width); t->h = MIN(t->h, t->dep.height); } unique_colors(t); return 1;
    }
    struct gl_framebuffer *f = gl_name_get(&framebuffers, t->fb); if (!f) return 0;
    id<MTLTexture> attachments[ATT_N] = {nil}; uint32_t cids[GL_COLOR_TARGETS] = {0}; int any = 0;
    for (int i = 0; i < ATT_N; i++) attachments[i] = att_tex(&f->att[i]);
    GLenum status = fb_status(f, attachments);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        GLRED1("帧缓冲 %u 不完整 / 不支持（%#x）", t->fb, status); gl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return 0;
    }
    id<MTLTexture> d = attachments[ATT_DEPTH], s = attachments[ATT_STEN];
    // 宽屏（gl_widen.inc）：附件换成放宽的影子贴图，游戏的纹理 / 渲染缓冲对象不变；关时原样
    for (int i = 0; i < GL_COLOR_TARGETS; i++) if (attachments[i])
        attachments[i] = gl_wd_att(att_wd(&f->att[i]), attachments[i], &cids[i]);
    if (d && d == s) d = s = gl_wd_att(att_wd(&f->att[ATT_DEPTH]), d, &t->did);
    else {
        if (d) d = gl_wd_att(att_wd(&f->att[ATT_DEPTH]), d, &t->did);
        uint32_t sid = 0;
        if (s) s = gl_wd_att(att_wd(&f->att[ATT_STEN]), s, &sid);
        if (!d) t->did = sid;
    }
    // 只绑定 GL 指定的平面；packed 单平面不自动激活未绑定的另一面。
    if (d && s && d != s && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
        if (f->stencil_parent != s || !f->stencil_view) {
            f->stencil_view = nil; f->stencil_parent = s;
            f->stencil_view = [s newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        }
        if (!f->stencil_view) { GLRED1("模板平面视图分配失败"); gl_set_error(GL_OUT_OF_MEMORY); return 0; }
        s = f->stencil_view;
    } else f->stencil_view = f->stencil_parent = nil;
    attachments[ATT_DEPTH] = d; attachments[ATT_STEN] = s;
    // GL 4.5：有效区域是全部附件的交集，路由为 NONE 也不扩大区域。
    for (int i = 0; i < ATT_N; i++) if (attachments[i]) {
        if (!any) { t->w = attachments[i].width; t->h = attachments[i].height; }
        else { t->w = MIN(t->w, attachments[i].width); t->h = MIN(t->h, attachments[i].height); } any = 1;
    }
    for (int i = 0; i < GL_COLOR_TARGETS; i++) if (f->draw[i] != GL_NONE) {
        int a = (int)(f->draw[i] - GL_COLOR_ATTACHMENT0); t->col[i] = attachments[a]; t->cid[i] = cids[a];
    }
    t->dep = d; t->sten = s; unique_colors(t); return 1;
}

// ---------- 帧缓冲之间拷贝 / 读回（glBlitFramebuffer、glReadPixels：FINAL FANTASY VII 启动即调） ----------
// 一端解析成颜色 / 深度贴图（复用 gl_draw_target：宽屏影子、窗口表面、按本次提交登记使用都在里面），gw = 游戏看到的颜色宽（换算横坐标：宽屏时贴图比游戏宽）
// 读端：读缓冲 = glReadBuffer 选的那路（帧缓冲对象：颜色附件 i，初值 0；0 号帧缓冲：后缓冲，GL_FRONT 读上一次交换交出去的那张；GL_NONE：没有颜色）
typedef struct { __strong id<MTLTexture> col, dep; NSUInteger gw; } fb_side;
static int read_front(GLenum b) { return b == 0x0404 || b == 0x0400 || b == 0x0406; }   // FRONT / FRONT_LEFT / LEFT（读时 LEFT 指前左）
static int fb_resolve(int read, fb_side *o) {
    GLuint fb = read ? gl_cur->fb_read : gl_cur->fb_draw, save = gl_cur->fb_draw;
    struct gl_framebuffer *f = fb ? gl_name_get(&framebuffers, fb) : NULL; if (fb && !f) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    GLenum route = f ? f->draw[0] : 0, dflt = gl_cur->default_draw[0];
    if (read) { if (f) f->draw[0] = f->read; else gl_cur->default_draw[0] = gl_cur->default_read == GL_NONE ? GL_NONE : GL_BACK; }
    gl_cur->fb_draw = fb; gl_target t; int ok = gl_draw_target(&t);
    gl_cur->fb_draw = save; if (read) { if (f) f->draw[0] = route; else gl_cur->default_draw[0] = dflt; }
    if (!ok) return 0;
    *o = (fb_side){0}; o->col = t.col[0]; o->dep = t.dep;
    if (read && !fb && read_front(gl_cur->default_read)) o->col = gl_surface_front();
    if (!fb) { uint32_t w, h; gl_surface_game(&w, &h); o->gw = w; }
    else { GLenum r = read ? f->read : f->draw[0]; int a = color_attachment(r) ? (int)(r - GL_COLOR_ATTACHMENT0) : -1; id<MTLTexture> g = a >= 0 ? att_tex(&f->att[a]) : nil; o->gw = g ? g.width : 0; }
    if (!o->gw && o->col) o->gw = o->col.width;
    return 1;
}
static double fb_kx(const fb_side *s, id<MTLTexture> t) { return s->gw && t ? (double)t.width / (double)s->gw : 1; }   // 横向放宽倍率（宽屏关 = 1）
static id<MTLRenderPipelineState> blit_pso(MTLPixelFormat pf) {
    static NSMutableDictionary *cache; static id<MTLLibrary> lib;
    if (!lib) {
        NSError *e = nil;
        lib = [gl_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V gl_blit_v(uint i [[vertex_id]], constant float4 *q [[buffer(0)]]) { float2 c = float2(i & 1, i >> 1); V o; o.p = float4(mix(q[0].xy, q[0].zw, c), 0, 1); o.uv = mix(q[1].xy, q[1].zw, c); return o; }\n"
            "fragment float4 gl_blit_f(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, v.uv); }\n" options:nil error:&e];
        if (!lib) { GLOG("【红】 glBlitFramebuffer 着色器编不过：%s", e.localizedDescription.UTF8String); return nil; }
        cache = [NSMutableDictionary new];
    }
    id hit = cache[@(pf)]; if (hit) return hit == NSNull.null ? nil : hit;   // 建失败存 NSNull：同一组合不再每笔重建（11-01）
    id<MTLRenderPipelineState> ps;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"gl_blit_v"]; d.fragmentFunction = [lib newFunctionWithName:@"gl_blit_f"]; d.colorAttachments[0].pixelFormat = pf;
    NSError *e = nil; ps = [gl_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { GLOG("【红】 glBlitFramebuffer 管线（格式 %lu）建不出：%s", (unsigned long)pf, e.localizedDescription.UTF8String); cache[@(pf)] = NSNull.null; return nil; }
    cache[@(pf)] = ps; return ps;
}
// 颜色：同尺寸、不镜像、同格式、同采样数、在界内 → 拷贝编码器；否则画一个四边形（缩放按 filter 取样，镜像靠反着的 uv）。坐标：本层贴图按 GL 朝向存（第 0 行 = GL 的 y = 0），不翻转
static void blit_color(id<MTLTexture> st, id<MTLTexture> dt, const double r[4], const double w[4], int linear) {
    long rx0 = lround(r[0]), ry0 = lround(r[1]), rx1 = lround(r[2]), ry1 = lround(r[3]), wx0 = lround(w[0]), wy0 = lround(w[1]), wx1 = lround(w[2]), wy1 = lround(w[3]);
    if (rx0 < rx1 && ry0 < ry1 && wx0 < wx1 && wy0 < wy1 && rx1 - rx0 == wx1 - wx0 && ry1 - ry0 == wy1 - wy0 && st.pixelFormat == dt.pixelFormat && st.sampleCount == dt.sampleCount &&
        rx0 >= 0 && ry0 >= 0 && wx0 >= 0 && wy0 >= 0 && (NSUInteger)rx1 <= st.width && (NSUInteger)ry1 <= st.height && (NSUInteger)wx1 <= dt.width && (NSUInteger)wy1 <= dt.height) {
        [gl_blit_enc() copyFromTexture:st sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)rx0, (NSUInteger)ry0, 0) sourceSize:MTLSizeMake((NSUInteger)(rx1 - rx0), (NSUInteger)(ry1 - ry0), 1)
                             toTexture:dt destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake((NSUInteger)wx0, (NSUInteger)wy0, 0)];
        return;
    }
    if (st.sampleCount > 1 && dt.sampleCount == 1) {   // 解析（多重采样渲染缓冲 → 单采样）：GL 4.5 §18.3.1 要求源与目标矩形同尺寸（可镜像），否则 INVALID_OPERATION
        if (labs(rx1 - rx0) != labs(wx1 - wx0) || labs(ry1 - ry0) != labs(wy1 - wy0)) { gl_set_error(GL_INVALID_OPERATION); return; }
        static id<MTLTexture> tmp;   // 整张解析进同尺寸同格式的单采样暂存，再按单采样路径拷 / 画到目标（换格式、镜像、宽屏横向倍率都走那条路）
        if (!tmp || tmp.width != st.width || tmp.height != st.height || tmp.pixelFormat != st.pixelFormat) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:st.pixelFormat width:st.width height:st.height mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; tmp = [gl_dev newTextureWithDescriptor:d];
            if (!tmp) { GLOG("【红】 glBlitFramebuffer 解析暂存分配失败"); gl_set_error(GL_OUT_OF_MEMORY); return; }
        }
        gl_blit_enc(); gl_end_enc();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = st; rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; rp.colorAttachments[0].resolveTexture = tmp;
        [[gl_cmd() renderCommandEncoderWithDescriptor:rp] endEncoding];   // 空遍：载入多重采样内容、存回并解析
        blit_color(tmp, dt, r, w, linear); return;
    }
    if (st.sampleCount > 1 || dt.sampleCount > 1) { GLRED1("glBlitFramebuffer：写进多重采样目标的缩放 / 换格式拷贝没实现"); gl_bad_feat++; return; }
    id<MTLRenderPipelineState> ps = blit_pso(dt.pixelFormat); if (!ps) { gl_bad_feat++; return; }
    static id<MTLSamplerState> smp[2];
    if (!smp[linear]) { MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; smp[linear] = [gl_dev newSamplerStateWithDescriptor:sd]; }
    gl_blit_enc(); gl_end_enc();   // 兑现挂起清除、收掉开着的编码器
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dt; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [gl_cmd() renderCommandEncoderWithDescriptor:rp];
    const double W = (double)dt.width, H = (double)dt.height, sw = (double)st.width, sh = (double)st.height;
    float q[8] = {(float)(2 * w[0] / W - 1), (float)(1 - 2 * w[1] / H), (float)(2 * w[2] / W - 1), (float)(1 - 2 * w[3] / H),   // 目标：第 y 行 → NDC 1 − 2y / H
                  (float)(r[0] / sw), (float)(r[1] / sh), (float)(r[2] / sw), (float)(r[3] / sh)};
    [re setRenderPipelineState:ps]; [re setVertexBytes:q length:sizeof q atIndex:0];
    [re setFragmentTexture:st atIndex:0]; [re setFragmentSamplerState:smp[linear] atIndex:0];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [re endEncoding];
}
// 第 9、10 个参数在栈上：Switch（标准 AAPCS64）每个栈参数占 8 字节一格（mask 在 sp+0、filter 在 sp+8），Apple ARM64 把 32 位栈参数紧挨着排（sp+0 / sp+4）。
//   照 32 位声明，宿主把 mask 那格的高 32 位（0）当 filter 读 → 判非法枚举（FINAL FANTASY VII 11-01：GL 错误 0x500 由 glBlitFramebuffer 置、游戏中止）。声明成 64 位按 8 字节一格读，再截回 32 位
GLB(glBlitFramebuffer, void, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, uint64_t mask64, uint64_t filter64) { @autoreleasepool {
    CTX(); GLbitfield mask = (GLbitfield)mask64; GLenum filter = (GLenum)filter64;
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) { gl_set_error(GL_INVALID_VALUE); return; }
    if (filter != 0x2600 && filter != 0x2601) { gl_set_error(GL_INVALID_ENUM); return; }   // NEAREST / LINEAR
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && filter != 0x2600) { gl_set_error(GL_INVALID_OPERATION); return; }
    fb_side rs, ws; if (!fb_resolve(1, &rs) || !fb_resolve(0, &ws)) return;
    if (mask & GL_COLOR_BUFFER_BIT) {
        if (!rs.col || !ws.col) GLRED1("glBlitFramebuffer：读端或写端没有颜色附件，颜色不拷");
        else { double kr = fb_kx(&rs, rs.col), kw = fb_kx(&ws, ws.col);
            double r[4] = {sx0 * kr, sy0, sx1 * kr, sy1}, w[4] = {dx0 * kw, dy0, dx1 * kw, dy1};
            gl_dev_init(); @autoreleasepool { blit_color(rs.col, ws.col, r, w, filter == 0x2601); } }
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        id<MTLTexture> sd = rs.dep, dd = ws.dep;
        if (!sd || !dd) GLRED1("glBlitFramebuffer：读端或写端没有深度附件，深度不拷");
        else if (sx1 - sx0 != dx1 - dx0 || sy1 - sy0 != dy1 - dy0 || sx0 > sx1 || sy0 > sy1 || sd.pixelFormat != dd.pixelFormat || sd.sampleCount != dd.sampleCount || sx0 < 0 || sy0 < 0 || dx0 < 0 || dy0 < 0 ||
                 (NSUInteger)sx1 > sd.width || (NSUInteger)sy1 > sd.height || (NSUInteger)dx1 > dd.width || (NSUInteger)dy1 > dd.height) { GLRED1("glBlitFramebuffer：深度只实现了同尺寸、同格式、不镜像的拷贝"); gl_bad_feat++; }
        else [gl_blit_enc() copyFromTexture:sd sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)sx0, (NSUInteger)sy0, 0) sourceSize:MTLSizeMake((NSUInteger)(sx1 - sx0), (NSUInteger)(sy1 - sy0), 1)
                                    toTexture:dd destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake((NSUInteger)dx0, (NSUInteger)dy0, 0)];
    }
    if (mask & GL_STENCIL_BUFFER_BIT) { GLRED1("glBlitFramebuffer：模板拷贝没实现"); gl_bad_feat++; }
}}
// 读回：只实现 GL_RGBA + GL_UNSIGNED_BYTE（读 RGBA8 / BGRA8 颜色）；先提交并等 GPU 做完。行序同 GL（第 0 行 = y），宽屏时横向按放宽倍率取最近的列
GLB(glReadPixels, void, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data) { @autoreleasepool {
    CTX();
    if (w < 0 || h < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (format != 0x1908 || type != 0x1401) { GLRED1("glReadPixels 格式 %#x / 类型 %#x 没实现（只有 RGBA / UNSIGNED_BYTE）", format, type); gl_set_error(GL_INVALID_OPERATION); return; }
    // 绑着像素打包缓冲：data 是缓冲内偏移，读回写进缓冲（GL 4.5 §18.2 / ES 3.0 §4.3.2）。缓冲映射着（持久映射除外）或放不下 → INVALID_OPERATION
    if (gl_pack_buffer_name()) {
        struct gl_buffer *pb = gl_pack_buffer(); size_t off = (size_t)(uintptr_t)data, need = (size_t)w * (size_t)h * 4;
        if (!pb || !pb->buf || (pb->map_access && !(pb->map_bits & 0x40)) || off > pb->size || need > pb->size - off) { gl_set_error(GL_INVALID_OPERATION); return; }
        data = (uint8_t *)pb->buf.contents + off; pb->idx16 = nil; pb->fan32_cache = nil;   // 写进新内容：索引副本失效（同 BufferSubData）
    }
    if (!w || !h || !data) return;
    fb_side rs; if (!fb_resolve(1, &rs)) return;
    id<MTLTexture> t = rs.col; if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }   // 读缓冲 GL_NONE / 没有附件（规范：INVALID_OPERATION）
    MTLPixelFormat pf = t.pixelFormat; int bgra = pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!bgra && pf != MTLPixelFormatRGBA8Unorm && pf != MTLPixelFormatRGBA8Unorm_sRGB) { GLRED1("glReadPixels：读缓冲格式 %lu 没实现（只有 RGBA8 / BGRA8）", (unsigned long)pf); gl_set_error(GL_INVALID_OPERATION); return; }
    double k = fb_kx(&rs, t);
    long x0 = lround(x * k), x1 = lround((x + w) * k); if (x0 < 0) x0 = 0; if (x1 > (long)t.width) x1 = (long)t.width;
    long y0 = y < 0 ? 0 : y, y1 = y + h > (long)t.height ? (long)t.height : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    NSUInteger tw = (NSUInteger)(x1 - x0), th = (NSUInteger)(y1 - y0);
    gl_dev_init();
    @autoreleasepool {
        id<MTLBuffer> b = [gl_dev newBufferWithLength:tw * th * 4 options:MTLResourceStorageModeShared];
        if (!b) { gl_set_error(GL_OUT_OF_MEMORY); return; }
        [gl_blit_enc() copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0) sourceSize:MTLSizeMake(tw, th, 1)
                              toBuffer:b destinationOffset:0 destinationBytesPerRow:tw * 4 destinationBytesPerImage:tw * th * 4];
        gl_wait(gl_commit());
        const uint8_t *src = b.contents; uint8_t *out = data;
        for (long j = y0; j < y1; j++) for (long i = 0; i < w; i++) {
            long c = lround((x + i) * k) - x0; if (c < 0 || c >= (long)tw) continue;
            const uint8_t *s = src + ((size_t)(j - y0) * tw + (size_t)c) * 4; uint8_t *d = out + ((size_t)(j - y) * (size_t)w + (size_t)i) * 4;
            d[0] = s[bgra ? 2 : 0]; d[1] = s[1]; d[2] = s[bgra ? 0 : 2]; d[3] = s[3];
        }
    }
}}

// ---------- 状态查询 ----------
// 驱动身份与扩展表照上游参考实现（游戏实测走得通的取值）：NVIDIA 桌面 GL 4.5，扩展只报本层真支持的压缩格式与对象模型
// 版本串按当前上下文的 API 报（真机 NVIDIA 驱动同一份实现两种上下文）：GLES 3.2 §6.1.6 规定 ES 上下文的 VERSION 为「OpenGL ES N.M 厂商信息」、
//   SHADING_LANGUAGE_VERSION 为「OpenGL ES GLSL ES N.M 厂商信息」，MAJOR / MINOR_VERSION 为 3 / 2。11-03 火影羁绊：Scaleform（GLES 版）按
//   sscanf("OpenGL ES %d.%d") 取版本，读到桌面串得 0.0，核心函数（glGenFramebuffers / glBindFramebuffer 等）的指针一律不取、包装层见空直接返回，
//   渲染目标落到无表面上下文的 0 号帧缓冲（每帧清除报 0x506），十几帧后 Scaleform 释放链上崩（main+0xcd0754 空闲链读 0x8）；按 ES 报后同包本机局不崩
static int gl_es(void) { return gl_cur->api == 0x30A0; }   // EGL_OPENGL_ES_API
static const char *const k_exts[] = {"GL_ARB_ES3_2_compatibility", "GL_ARB_ES3_compatibility", "GL_ARB_texture_compression_bptc", "GL_ARB_texture_compression_rgtc",
    "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_filter_anisotropic", "GL_ARB_vertex_array_object", "GL_ARB_direct_state_access", "GL_ARB_half_float_vertex",
    "GL_ARB_framebuffer_object", "GL_ARB_texture_float", "GL_EXT_texture_sRGB", "GL_KHR_texture_compression_astc_ldr", "GL_NV_shadow_samplers_cube"};
enum { NEXTS = sizeof k_exts / sizeof *k_exts };
// 交出的串（静态区地址，游戏可能拷走指针）：按「gl:str:<名>:<es>」/「gl:str:i:<号>」登记身份，恢复时 gl_snap_str 按身份找回本进程的串
static const char *gl_str_of(GLenum name, int es) {
    switch (name) {
        case 0x1F00: return "NVIDIA Corporation";                              // VENDOR
        case 0x1F01: return "NVIDIA Tegra X1 (nvgpu)/integrated (Metal)";      // RENDERER
        case 0x1F02: return es ? "OpenGL ES 3.2 NVIDIA 349.00" : "4.5.0 NVIDIA 349.00";      // VERSION
        case 0x8B8C: return es ? "OpenGL ES GLSL ES 3.20" : "4.50 NVIDIA";                   // SHADING_LANGUAGE_VERSION
        case 0x1F03: {                                                          // EXTENSIONS：兼容配置的整串
            static char all[1024]; if (!all[0]) for (int i = 0; i < NEXTS; i++) { strlcat(all, k_exts[i], sizeof all); strlcat(all, " ", sizeof all); }
            return all;
        }
    }
    return NULL;
}
void s2a_snap_handout_id(uint64_t v, const char *id) __attribute__((weak));
int s2a_snap_mode(void) __attribute__((weak));
static void str_ho(const char *p, const char *fmt, unsigned a, unsigned b) {
    if (!p || !s2a_snap_handout_id || !s2a_snap_mode || !s2a_snap_mode()) return;
    char id[48]; snprintf(id, sizeof id, fmt, a, b); s2a_snap_handout_id((uint64_t)(uintptr_t)p, id);
}
const char *gl_snap_str(const char *id) {
    unsigned a = 0, b = 0;
    if (sscanf(id, "i:%u", &a) == 1) return a < NEXTS ? k_exts[a] : NULL;
    return sscanf(id, "%x:%u", &a, &b) == 2 ? gl_str_of((GLenum)a, (int)b) : NULL;
}
GL(glGetString, const GLchar *, GLenum name) {
    CTX(NULL);
    const char *r = gl_str_of(name, gl_es()); if (r) { str_ho(r, "gl:str:%x:%u", name, (unsigned)gl_es()); return r; }
    gl_set_error(GL_INVALID_ENUM); return NULL;
}
GL(glGetStringi, const GLchar *, GLenum name, GLuint i) {
    CTX(NULL); if (name != 0x1F03) { gl_set_error(GL_INVALID_ENUM); return NULL; }
    if (i >= NEXTS) { gl_set_error(GL_INVALID_VALUE); return NULL; }
    str_ho(k_exts[i], "gl:str:i:%u", i, 0); return k_exts[i];
}
// 每个 draw buffer 的颜色状态；普通查询取第 0 路，indexed 查询按 i 取。
static int color_query(GLenum p, GLuint i, double v[4]) {
    const gl_color_state *c = &gl_cur->color[i];
    switch (p) {
        case GL_BLEND: v[0] = c->blend; return 1;
        case 0x0C23: for (int k = 0; k < 4; k++) v[k] = c->mask[k]; return 4;   // COLOR_WRITEMASK
        case 0x8009: v[0] = c->eq[0]; return 1; case 0x883D: v[0] = c->eq[1]; return 1;   // BLEND_EQUATION_RGB / ALPHA
        case 0x0BE1: case 0x80C9: v[0] = c->factor[0]; return 1;   // BLEND_SRC / SRC_RGB
        case 0x0BE0: case 0x80C8: v[0] = c->factor[1]; return 1;   // BLEND_DST / DST_RGB
        case 0x80CB: v[0] = c->factor[2]; return 1; case 0x80CA: v[0] = c->factor[3]; return 1;   // BLEND_SRC_ALPHA / DST_ALPHA
    }
    return 0;
}
// 当前绘制帧缓冲的采样数（SAMPLE_BUFFERS / SAMPLES）：0 号帧缓冲是单采样窗口表面；帧缓冲对象取第一个有存储的附件（同一帧缓冲的附件采样数必须一致，否则不完整）
static int draw_samples(void) {
    if (!gl_cur->fb_draw) return 1;
    struct gl_framebuffer *f = gl_name_get(&framebuffers, gl_cur->fb_draw); if (!f) return 1;
    for (int k = 0; k < ATT_N; k++) if (f->att[k].kind == 2) { gl_renderbuffer *r = gl_name_get(&renderbuffers, f->att[k].name); if (r && r->tex) return (int)r->tex.sampleCount; }
    return 1;   // 纹理附件：本层没有多采样纹理
}
// 整数与浮点查询共用一张表：返回分量个数（0 = 没实现），值写进 v（double 不丢 uint32 掩码精度）
static int query(GLenum p, double v[4]) {
    int n = color_query(p, 0, v); if (n) return n;
    if (p == 0x0C01 || (p >= 0x8825 && p < 0x8825 + GL_COLOR_TARGETS)) {   // DRAW_BUFFER / DRAW_BUFFERi
        GLenum *route = draw_routes(); if (!route) return 0; v[0] = route[p == 0x0C01 ? 0 : p - 0x8825]; return 1;
    }
    switch (p) {
        case 0x821D: v[0] = NEXTS; return 1;                                    // NUM_EXTENSIONS
        // 剔除 / 模板状态读回（FINAL FANTASY VII 启动时逐个读，查不到置 GL_INVALID_ENUM，游戏 glGetError 见错即中止）：正面 [0]、背面 [1]
        case 0x0B21: v[0] = gl_cur->line_width; return 1;   // LINE_WIDTH
        case 0x0B45: v[0] = gl_cur->cull; return 1; case 0x0B46: v[0] = gl_cur->front; return 1;               // CULL_FACE_MODE / FRONT_FACE
        case 0x0B92: v[0] = gl_cur->stencil_func[0]; return 1; case 0x8800: v[0] = gl_cur->stencil_func[1]; return 1;   // STENCIL_(BACK_)FUNC
        case 0x0B93: v[0] = gl_cur->stencil_mask[0]; return 1; case 0x8CA4: v[0] = gl_cur->stencil_mask[1]; return 1;   // STENCIL_(BACK_)VALUE_MASK
        case 0x0B97: v[0] = gl_cur->stencil_ref[0]; return 1; case 0x8CA3: v[0] = gl_cur->stencil_ref[1]; return 1;     // STENCIL_(BACK_)REF
        case 0x0B94: v[0] = gl_cur->stencil_op[0][0]; return 1; case 0x8801: v[0] = gl_cur->stencil_op[1][0]; return 1; // STENCIL_(BACK_)FAIL
        case 0x0B95: v[0] = gl_cur->stencil_op[0][1]; return 1; case 0x8802: v[0] = gl_cur->stencil_op[1][1]; return 1; // STENCIL_(BACK_)PASS_DEPTH_FAIL
        case 0x0B96: v[0] = gl_cur->stencil_op[0][2]; return 1; case 0x8803: v[0] = gl_cur->stencil_op[1][2]; return 1; // STENCIL_(BACK_)PASS_DEPTH_PASS
        case 0x821B: v[0] = gl_es() ? 3 : 4; return 1; case 0x821C: v[0] = gl_es() ? 2 : 5; return 1;   // MAJOR / MINOR_VERSION（ES 3.2 / GL 4.5）
        case 0x0D33: case 0x84E8: v[0] = 16384; return 1;                       // MAX_TEXTURE_SIZE / MAX_RENDERBUFFER_SIZE
        case 0x851C: v[0] = 16384; return 1;                                    // MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x8869: v[0] = 16; return 1;                                       // MAX_VERTEX_ATTRIBS
        case 0x8872: case 0x8B4C: v[0] = 16; return 1;                          // MAX_TEXTURE_IMAGE_UNITS / MAX_VERTEX_TEXTURE_IMAGE_UNITS（Metal 每阶段 16 个采样器）
        case 0x8B4D: v[0] = 32; return 1;                                       // MAX_COMBINED_TEXTURE_IMAGE_UNITS
        case 0x8DFB: case 0x8DFD: v[0] = 1024; return 1;                        // MAX_VERTEX / FRAGMENT_UNIFORM_VECTORS
        case 0x8B4A: case 0x8B49: v[0] = 4096; return 1;                        // MAX_VERTEX / FRAGMENT_UNIFORM_COMPONENTS
        case 0x8DFC: v[0] = 31; return 1;                                       // MAX_VARYING_VECTORS
        case 0x8CDF: case 0x8824: v[0] = GL_COLOR_TARGETS; return 1;             // MAX_COLOR_ATTACHMENTS / MAX_DRAW_BUFFERS
        case 0x8D57: v[0] = 8; return 1;                                        // MAX_SAMPLES
        case 0x84FD: v[0] = 15; return 1;                                       // MAX_TEXTURE_LOD_BIAS
        case 0x84FF: v[0] = 16; return 1;                                       // MAX_TEXTURE_MAX_ANISOTROPY
        case 0x0D3A: v[0] = v[1] = 16384; return 2;                             // MAX_VIEWPORT_DIMS
        case 0x0BA2: for (int i = 0; i < 4; i++) v[i] = gl_cur->vp[i]; return 4;    // VIEWPORT
        case 0x0C10: for (int i = 0; i < 4; i++) v[i] = gl_cur->sc[i]; return 4;    // SCISSOR_BOX
        case 0x0B70: v[0] = gl_cur->depth_range[0]; v[1] = gl_cur->depth_range[1]; return 2;   // DEPTH_RANGE
        case 0x0C22: for (int i = 0; i < 4; i++) v[i] = gl_cur->clear_color[i]; return 4;      // COLOR_CLEAR_VALUE
        case 0x0B73: v[0] = gl_cur->clear_depth; return 1;                      // DEPTH_CLEAR_VALUE
        case 0x8CA6: v[0] = gl_cur->fb_draw; return 1; case 0x8CAA: v[0] = gl_cur->fb_read; return 1;   // DRAW / READ_FRAMEBUFFER_BINDING
        case 0x8CA7: v[0] = gl_cur->rb_bound; return 1;                         // RENDERBUFFER_BINDING
        case 0x8B8D: v[0] = gl_cur->prog; return 1;                             // CURRENT_PROGRAM
        case 0x85B5: v[0] = gl_cur->vao; return 1;                              // VERTEX_ARRAY_BINDING
        case 0x8894: v[0] = gl_cur->buf_bound[0]; return 1;                     // ARRAY_BUFFER_BINDING
        case 0x88ED: v[0] = gl_pack_buffer_name(); return 1;                    // PIXEL_PACK_BUFFER_BINDING
        case 0x88EF: v[0] = gl_unpack_buffer_name(); return 1;                  // PIXEL_UNPACK_BUFFER_BINDING
        case 0x84E0: v[0] = 0x84C0 + gl_cur->active_unit; return 1;             // ACTIVE_TEXTURE
        case 0x8069: v[0] = gl_cur->tex_unit[gl_cur->active_unit][0]; return 1; // TEXTURE_BINDING_2D
        case 0x8514: v[0] = gl_cur->tex_unit[gl_cur->active_unit][1]; return 1; // TEXTURE_BINDING_CUBE_MAP
        case 0x0CF5: v[0] = gl_cur->unpack_align; return 1; case 0x0CF2: v[0] = gl_cur->unpack_row; return 1;   // UNPACK_ALIGNMENT / ROW_LENGTH
        case 0x0D05: v[0] = gl_cur->pack_align; return 1;                       // PACK_ALIGNMENT
        case 0x0B74: v[0] = gl_cur->depth_func; return 1; case 0x0B72: v[0] = gl_cur->depth_mask; return 1;   // DEPTH_FUNC / DEPTH_WRITEMASK
        case 0x0B98: v[0] = gl_cur->stencil_wmask[0]; return 1;                 // STENCIL_WRITEMASK
        case 0x8005: for (int i = 0; i < 4; i++) v[i] = gl_cur->blend_color[i]; return 4;    // BLEND_COLOR
        case 0x8038: v[0] = gl_cur->poly_factor; return 1; case 0x2A00: v[0] = gl_cur->poly_units; return 1;   // POLYGON_OFFSET_FACTOR / UNITS
        case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; return 1; // RED / GREEN / BLUE / ALPHA_BITS（原先错写成 0x0D50–0x0D53，蓝 / 透明位查不到）
        case 0x0D50: v[0] = 8; return 1; case 0x0D51: v[0] = 0; return 1;       // SUBPIXEL_BITS / INDEX_BITS（RGBA 模式没有颜色索引）
        case 0x0D56: v[0] = 24; return 1; case 0x0D57: v[0] = 8; return 1;      // DEPTH_BITS / STENCIL_BITS
        case 0x86A2: v[0] = 0; return 1;                                        // NUM_COMPRESSED_TEXTURE_FORMATS（逐个格式按扩展认）
        case 0x826E: v[0] = 1024; return 1;                                     // MAX_UNIFORM_LOCATIONS
        case 0x82DA: v[0] = 16; return 1;                                       // MAX_VERTEX_ATTRIB_BINDINGS
        case 0x8A34: v[0] = 256; return 1;                                      // UNIFORM_BUFFER_OFFSET_ALIGNMENT（Metal 常量缓冲偏移按 256 对齐；NVIDIA 同值）
        case 0x8DFA: v[0] = 1; return 1;                                        // SHADER_COMPILER（ES：支持 glCompileShader，源码按离线库取）
        // 以下 Balatro（LÖVE 引擎）建上下文时逐个查，原先没实现回 0 并置 GL_INVALID_ENUM
        case 0x80A8: { int s = draw_samples(); v[0] = s > 1; return 1; }       // SAMPLE_BUFFERS
        case 0x80A9: { int s = draw_samples(); v[0] = s > 1 ? s : 0; return 1; }   // SAMPLES（单采样报 0）
        case 0x8073: v[0] = 2048; return 1;                                     // MAX_3D_TEXTURE_SIZE（Metal 3D 纹理每维上限 2048）
        case 0x88FF: v[0] = 2048; return 1;                                     // MAX_ARRAY_TEXTURE_LAYERS（Metal 纹理数组上限 2048 层）
        case 0x0B11: v[0] = gl_cur->point_size; return 1;                       // POINT_SIZE
        case 0x0B12: case 0x846D: v[0] = 1; v[1] = 511; return 2;               // POINT_SIZE_RANGE（桌面）/ ALIASED_POINT_SIZE_RANGE：Metal [[point_size]] 上限 511
    }
    if (p == 0x9126 && !gl_es()) { v[0] = 1; return 1; }                        // CONTEXT_PROFILE_MASK：core（本层没有固定管线 / 显示列表这些兼容配置的入口）；ES 没有这个查询
    return 0;
}
static void query_red(const char *fn, GLenum p) {
    static GLenum seen[64]; static int n; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); int known = 0; for (int i = 0; i < n; i++) known |= seen[i] == p;
    if (!known && n < 64) { seen[n++] = p; GLOG("【红】 %s(%#x) 没实现（回 0）", fn, p); }
    pthread_mutex_unlock(&mu);
}
GL(glGetIntegerv, void, GLenum p, GLint *v) {
    CTX(); double d[4]; int n = query(p, d);
    if (!n) { query_red("glGetIntegerv", p); gl_set_error(GL_INVALID_ENUM); *v = 0; return; }
    // 颜色 / 深度类浮点值按规范线性映射到整数全域；其余取整
    int norm = p == 0x0C22 || p == 0x0B73 || p == 0x0B70 || p == 0x8005;
    for (int i = 0; i < n; i++) v[i] = norm ? (GLint)llround(d[i] * 2147483647.0) : (GLint)(int64_t)d[i];
}
GL(glGetFloatv, void, GLenum p, GLfloat *v) {
    CTX(); double d[4]; int n = query(p, d);
    if (!n) { query_red("glGetFloatv", p); gl_set_error(GL_INVALID_ENUM); *v = 0; return; }
    for (int i = 0; i < n; i++) v[i] = (GLfloat)d[i];
}
GL(glGetBooleanv, void, GLenum p, GLboolean *v) {
    CTX(); double d[4]; int n = query(p, d);
    if (!n) { query_red("glGetBooleanv", p); gl_set_error(GL_INVALID_ENUM); return; }
    for (int i = 0; i < n; i++) v[i] = d[i] != 0;
}
static int query_indexed(GLenum p, GLuint i, double d[4]) {
    // 先辨认 pname，错误调用不写输出，也不允许越界取上下文状态。
    double probe[4]; if (!color_query(p, 0, probe)) { gl_set_error(GL_INVALID_ENUM); return 0; }
    if (!color_index(i)) return 0; return color_query(p, i, d);
}
GL(glGetIntegeri_v, void, GLenum p, GLuint i, GLint *v) {
    CTX(); double d[4]; int n = query_indexed(p, i, d); for (int k = 0; k < n; k++) v[k] = (GLint)d[k];
}
GL(glGetBooleani_v, void, GLenum p, GLuint i, GLboolean *v) {
    CTX(); double d[4]; int n = query_indexed(p, i, d); for (int k = 0; k < n; k++) v[k] = d[k] != 0;
}

// ---------- glReadBuffer ----------
// 帧缓冲对象：GL_NONE / COLOR_ATTACHMENTi（i < 8）；0 号帧缓冲：GL_NONE / 后（BACK、BACK_LEFT）/ 前（FRONT、FRONT_LEFT、LEFT）。没有立体缓冲：RIGHT 类报 INVALID_OPERATION（GL 4.5 §18.2.1）
GL(glReadBuffer, void, GLenum src) {
    CTX();
    if (gl_cur->fb_read) {
        struct gl_framebuffer *f = gl_name_get(&framebuffers, gl_cur->fb_read); if (!f) { gl_set_error(GL_INVALID_OPERATION); return; }
        if (src == GL_NONE || color_attachment(src)) { f->read = src; return; }
        gl_set_error(default_buffer(src) || (src >= GL_COLOR_ATTACHMENT0 && src < GL_COLOR_ATTACHMENT0 + 32) ? GL_INVALID_OPERATION : GL_INVALID_ENUM); return;
    }
    if (src == GL_NONE || src == GL_BACK || src == GL_BACK_LEFT || read_front(src)) { gl_cur->default_read = src; return; }
    gl_set_error(src == 0x0401 || src == 0x0403 || src == 0x0407 || color_attachment(src) ? GL_INVALID_OPERATION : GL_INVALID_ENUM);   // FRONT_RIGHT / BACK_RIGHT / RIGHT、颜色附件
}

// ---------- glGetFramebufferAttachmentParameteriv（火影忍者 终极风暴羁绊） ----------
// 附件的对象类型 / 名字 / 级 / 立方体面，以及图像的各通道位数、分量类型、颜色编码（按实际 Metal 存储格式报：24 位深度实际存 32 位浮点，报 32 / FLOAT）
static int rgb_internal(GLenum f) { return f == 0x8051 || f == 0x1907 || f == 0x8C41 || f == 0x881B || f == 0x8815 || f == 0x8D62; }   // RGB8 / RGB / SRGB8 / RGB16F / RGB32F / RGB565：alpha 0 位
static void fmt_bits(MTLPixelFormat pf, GLenum internal, int bits[6], GLenum *ctype, GLenum *enc) {
    memset(bits, 0, 6 * sizeof *bits); *ctype = 0x8C17; *enc = 0x2601;   // UNSIGNED_NORMALIZED / LINEAR
    switch (pf) {
        case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatBGRA8Unorm_sRGB: *enc = 0x8C40; /* SRGB */ // fall through
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatBGRA8Unorm: bits[0] = bits[1] = bits[2] = bits[3] = 8; break;
        case MTLPixelFormatR8Unorm: bits[0] = 8; break; case MTLPixelFormatRG8Unorm: bits[0] = bits[1] = 8; break;
        case MTLPixelFormatRGBA16Float: bits[0] = bits[1] = bits[2] = bits[3] = 16; *ctype = 0x1406; break;
        case MTLPixelFormatRGBA32Float: bits[0] = bits[1] = bits[2] = bits[3] = 32; *ctype = 0x1406; break;
        case MTLPixelFormatR16Float: bits[0] = 16; *ctype = 0x1406; break; case MTLPixelFormatR32Float: bits[0] = 32; *ctype = 0x1406; break;
        case MTLPixelFormatRG16Float: bits[0] = bits[1] = 16; *ctype = 0x1406; break; case MTLPixelFormatRG32Float: bits[0] = bits[1] = 32; *ctype = 0x1406; break;
        case MTLPixelFormatRG11B10Float: bits[0] = bits[1] = 11; bits[2] = 10; *ctype = 0x1406; break;
        case MTLPixelFormatRGB10A2Unorm: bits[0] = bits[1] = bits[2] = 10; bits[3] = 2; break;
        case MTLPixelFormatRGBA16Unorm: bits[0] = bits[1] = bits[2] = bits[3] = 16; break;
        case MTLPixelFormatDepth16Unorm: bits[4] = 16; break;
        case MTLPixelFormatDepth32Float: bits[4] = 32; *ctype = 0x1406; break;
        case MTLPixelFormatDepth32Float_Stencil8: bits[4] = 32; bits[5] = 8; *ctype = 0x1406; break;
        case MTLPixelFormatStencil8: bits[5] = 8; *ctype = 0x1405; break;   // UNSIGNED_INT
        default: GLRED1("glGetFramebufferAttachmentParameteriv：存储格式 %lu 的位数表没实现（回 0）", (unsigned long)pf); break;
    }
    if (rgb_internal(internal)) bits[3] = 0;
}
GL(glGetFramebufferAttachmentParameteriv, void, GLenum target, GLenum attachment, GLenum what, GLint *v) {
    CTX(); if (target != GL_FRAMEBUFFER && target != 0x8CA9 && target != 0x8CA8) { gl_set_error(GL_INVALID_ENUM); return; }
    GLuint fbn = target == 0x8CA8 ? gl_cur->fb_read : gl_cur->fb_draw;
    GLenum type = GL_NONE, internal = 0; GLuint name = 0; const gl_att *a = NULL; id<MTLTexture> img = nil; int stencil_plane = 0;
    if (!fbn) {   // 0 号帧缓冲：颜色（前 / 后、左）、GL_DEPTH、GL_STENCIL
        if (attachment == 0x1801 || attachment == 0x1802) { img = gl_surface_depth(); stencil_plane = attachment == 0x1802; }
        else if (attachment == GL_BACK || attachment == GL_BACK_LEFT || read_front(attachment)) { img = gl_surface_color(); internal = 0x8058; }
        else if (attachment == 0x0401 || attachment == 0x0403) img = nil;   // FRONT_RIGHT / BACK_RIGHT：没有立体缓冲，对象类型 NONE
        else { gl_set_error(color_attachment(attachment) || attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT ? GL_INVALID_OPERATION : GL_INVALID_ENUM); return; }
        if (img) type = 0x8218;   // FRAMEBUFFER_DEFAULT
    } else {
        struct gl_framebuffer *f = gl_name_get(&framebuffers, fbn); if (!f) { gl_set_error(GL_INVALID_OPERATION); return; }
        if (color_attachment(attachment)) a = &f->att[attachment - GL_COLOR_ATTACHMENT0];
        else if (attachment == GL_DEPTH_ATTACHMENT) a = &f->att[ATT_DEPTH];
        else if (attachment == GL_STENCIL_ATTACHMENT) { a = &f->att[ATT_STEN]; stencil_plane = 1; }
        else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            const gl_att *d = &f->att[ATT_DEPTH], *st = &f->att[ATT_STEN];
            if (d->kind != st->kind || d->name != st->name || d->level != st->level || d->target != st->target) { gl_set_error(GL_INVALID_OPERATION); return; }   // 深度与模板不是同一个对象
            a = d;
            if (what == 0x8211) { gl_set_error(GL_INVALID_OPERATION); return; }   // DEPTH_STENCIL 不许查分量类型（规范）
        }
        else { gl_set_error(attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 32 ? GL_INVALID_OPERATION : GL_INVALID_ENUM); return; }
        type = a->kind == 1 ? 0x1702 : a->kind == 2 ? GL_RENDERBUFFER : GL_NONE; name = a->kind ? a->name : 0;
        img = att_tex(a);
        if (a->kind == 2) { gl_renderbuffer *r = gl_name_get(&renderbuffers, a->name); internal = r ? r->format : 0; }
        else if (a->kind == 1) internal = gl_tex_level_format(a->name, a->target, a->level);
    }
    if (what == 0x8CD0) { *v = (GLint)type; return; }                       // OBJECT_TYPE
    if (what == 0x8CD1) { *v = (GLint)name; return; }                       // OBJECT_NAME
    if (type == GL_NONE) { gl_set_error(GL_INVALID_OPERATION); return; }    // 没有附件：只许查类型 / 名字
    if (what == 0x8CD2 || what == 0x8CD3 || what == 0x8CD4 || what == 0x8DA7) {   // TEXTURE_LEVEL / CUBE_MAP_FACE / LAYER / LAYERED：只对纹理附件
        if (type != 0x1702) { gl_set_error(GL_INVALID_ENUM); return; }
        *v = what == 0x8CD2 ? a->level : what == 0x8CD3 ? (a->target >= 0x8515 && a->target <= 0x851A ? (GLint)a->target : 0) : 0; return;
    }
    if ((what >= 0x8210 && what <= 0x8217)) {   // COLOR_ENCODING / COMPONENT_TYPE / R..A / DEPTH / STENCIL_SIZE
        int bits[6]; GLenum ctype, enc; if (!img) { *v = 0; return; }
        fmt_bits(img.pixelFormat, internal, bits, &ctype, &enc);
        if (what == 0x8210) *v = (GLint)enc; else if (what == 0x8211) *v = (GLint)(stencil_plane ? 0x1405 : ctype); else *v = bits[what - 0x8212];
        return;
    }
    GLRED1("glGetFramebufferAttachmentParameteriv 参数 %#x 没实现", what); gl_set_error(GL_INVALID_ENUM);
}

// ---------- 即时存档：帧缓冲对象（附件按「对象名字 + 级 / 面」，不含 Metal 贴图）与渲染缓冲（存储整张存） ----------
enum { GF_NAME = 1, GF_ATT, GF_DRAW, GF_READ, GF_USED, GR_NAME = 1, GR_FORMAT, GR_USED, GR_HAS };
int gl_fb_snap_save(FILE *f) {
    if (!nvn_snap_tex_save) return -1;
    sfw w; sfw_sec(&w, f, "gl.fbo", 1);
    for (GLuint i = 1; i < framebuffers.n; i++) { struct gl_framebuffer *b = framebuffers.v[i]; if (!b) continue;
        sfw_rec(&w); GLSF_W(&w, GF_NAME, i); GLSF_W(&w, GF_ATT, b->att); GLSF_W(&w, GF_DRAW, b->draw); GLSF_W(&w, GF_READ, b->read); GLSF_W(&w, GF_USED, b->used); sfw_end_rec(&w); }
    if (sfw_end(&w)) return -1;
    sfw_sec(&w, f, "gl.rbo", 1);
    for (GLuint i = 1; i < renderbuffers.n; i++) { gl_renderbuffer *b = renderbuffers.v[i]; if (!b) continue; int has = b->tex != nil;
        sfw_rec(&w); GLSF_W(&w, GR_NAME, i); GLSF_W(&w, GR_FORMAT, b->format); GLSF_W(&w, GR_USED, b->used); GLSF_W(&w, GR_HAS, has); sfw_end_rec(&w); }
    if (sfw_end(&w)) return -1;
    for (GLuint i = 1; i < renderbuffers.n; i++) { gl_renderbuffer *b = renderbuffers.v[i]; if (b && b->tex && nvn_snap_tex_save(f, (__bridge void *)b->tex) < 0) return -1; }
    return 0;
}
int gl_fb_snap_restore(FILE *f) {
    if (!nvn_snap_tex_load) return -1;
    sfr r; int bad = sfr_sec(&r, f, "gl.fbo", 1) != 0;
    while (!bad && sfr_rec(&r) == 1) { GLuint nm = 0; GLSF_R(&r, GF_NAME, nm); struct gl_framebuffer *b = calloc(1, sizeof *b); if (!nm || !b) { free(b); continue; }
        GLSF_RC(&r, GF_ATT, b->att, "gl.att", gl_att); GLSF_R(&r, GF_DRAW, b->draw); GLSF_R(&r, GF_READ, b->read); GLSF_R(&r, GF_USED, b->used);
        if (!gl_name_put(&framebuffers, nm, b)) { free(b); bad = 1; } }
    sfr_done(&r);
    if (bad || sfr_sec(&r, f, "gl.rbo", 1)) return -1;
    GLuint *has = NULL; uint32_t n = 0, cap = 0;
    while (sfr_rec(&r) == 1) { GLuint nm = 0; int h = 0; GLSF_R(&r, GR_NAME, nm); gl_renderbuffer *b = calloc(1, sizeof *b); if (!nm || !b) { free(b); continue; }
        GLSF_R(&r, GR_FORMAT, b->format); GLSF_R(&r, GR_USED, b->used); GLSF_R(&r, GR_HAS, h);
        if (!gl_name_put(&renderbuffers, nm, b)) { free(b); bad = 1; break; }
        if (h) { if (n == cap) { cap = cap ? cap * 2 : 64; has = realloc(has, cap * sizeof *has); } has[n++] = nm; } }
    sfr_done(&r);
    for (uint32_t i = 0; !bad && i < n; i++) { void *p = nvn_snap_tex_load(f); if (!p) { bad = 1; break; } gl_renderbuffer *b = gl_name_get(&renderbuffers, has[i]); b->tex = (__bridge_transfer id<MTLTexture>)p; }
    free(has);
    return bad ? -1 : 0;
}
void gl_fb_snap_fps(gl_fp_cb cb) { cb("gl.att", SNAP_FP_TYPE(gl_att)); }
