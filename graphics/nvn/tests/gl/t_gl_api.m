// GL 层接口补齐单测（11-01 全库接口清点：直连 GL 层缺 41 个函数）。从 gl_lookup 取真实实现，在真 Metal 设备上跑，GPU 读回核对；
// 着色器：同目录 tri.* / api_inst.* / api_uni.* / api_vol.*（合成 GLSL ES 3.20），t_gl_api.sh 用出包工具 gl_shaders.py 出库。每组开头取本组入口，取不到记一项败、本组跳过（补齐前的源码在这里红）。
// 期望值都不跑被测实现就能算：
// A 查询：64×64 目标、四边形盖住 NDC x ∈ [−1, 0]：像素中心 0.5…31.5 在内 → 32 × 64 = 2048 个样本；跨 glFlush 画两次 = 4096；局部清除不计；
//   ANY 类：没画 0、画了 1；生成图元数：6 个顶点 TRIANGLES = 2，3 个实例的 6 索引 = 6；经过时间 > 0 且 < 1 秒。
// B 同步：等到的栅栏之后，用另一条队列读回表面像素 = 栅栏前画的颜色（GPU 真做完）；错误参数按规范。
// C 读回：上传的已知字节原样读回；R8 宽 3 按 PACK_ALIGNMENT 4 每行 4 字节；GPU 清除写的颜色 / 深度（私有存储）读回 = 清除值；BC1 块字节原样。
// D 附件查询：RGBA8 纹理 → 类型 TEXTURE、名字、级、各 8 位；深度模板渲染缓冲 → 32 位浮点深度 + 8 位模板；RGB8 → alpha 0 位。
// E 体纹理：2D 数组各层 / 3D 各片纯色，按层号 / 片中心采样 = 该层颜色；在途 TexSubImage3D 前后两笔各读各的内容；GetTexImage 逐层字节。
// F 多重采样：4× 渲染缓冲画对角三角形、解析到单采样：内部纯绿、外部纯红、对角线穿过的像素是混色（单采样不可能出现）。
// G 映射：可写映射遇在途存储换新存储，前一笔仍画旧内容（左半红）、后一笔画新内容（右半绿）。
// H 实例：3 个实例按除数 1 的偏移落在 x = 16 / 32 / 48，当前通用属性 aK = 0.5 → 红 128。
// I 线框：GL_LINE 时三角形内部保持清除色、FILL 时填满；读缓冲：COLOR1 读到绿、COLOR0 读到红、GL_FRONT 读到上一帧交换出去的颜色。
// J uniform：ivec / mat2 / mat3（std140 每列 16 字节、transpose 行主序）写入值经着色器输出成颜色读回（÷255 精确或 ±1）；glIs* / 分离 / 校验 / 二进制着色器按规范。
// K 10:10:10:2 打包顶点属性（缺口族 43）：tri 程序的 aCol 用 GL_UNSIGNED_INT_2_10_10_10_REV / GL_INT_2_10_10_10_REV × 4 喂，三个竖条各画一笔、按 RGBA8 读回（±1）：
//   无符号归一化 (1023, 511, 0) → 1023/1023·255 = 255、511/1023·255 = 127.4 → 127、0；有符号归一化 (256, 511, −512) → 256/511·255 = 127.75 → 128、511/511 → 255、max(−512/511, −1) = −1 夹到 0；
//   不归一化 (1, 0, 0) 按 GL 应是 1.0 → 255，Metal 没有这种顶点格式，本层仍不画 → 保持清除色蓝（不许当归一化画成 1/1023 ≈ 0）
// L 逐帧换贴图存储不漏（11-01 街霸 30 周年合集：每帧把 384×224 的画面贴图重新指定一次再画到表面，Metal 已分配一路涨到分配失败）：
//   客体线程没有会排空的自动释放池（main 外层这个池同样到进程结束才排空），本层在其上建的命令缓冲 / 编码器 / 渲染遍描述若进了它，
//   就连同引用到的每帧新贴图一起活到线程结束。两种写法各跑 600 帧（glTexImage2D 每帧换存储；glTexSubImage2D 改在途贴图也换存储），
//   每帧采样那张贴图画满表面再 eglSwapBuffers。期望：预热 60 帧后再 600 帧，glFinish 等完、Metal 释放落定后，
//   设备已分配增量 ≈ 0（容差 8 MB，约 24 张 384×224×4 = 336 KB 的贴图），进程堆增量 ≈ 0（容差 32 MB：11-09 高负载 64 次实测堆抖动最大 3.7 MB，原容差 2 MB 误红；
//   事故本体是贴图被拽住、看设备已分配，堆这条仍能抓每帧 ≥ 55 KB 的堆漏）；两项都等读数落定再读
//   漏的话增量 ≥ 600 × 336 KB ≈ 197 MB（与次数成正比）。
// M 多线程共享上下文（11-02 火影忍者 终极风暴羁绊：另一线程建共享上下文、无表面 MakeCurrent 后渲染线程清除落到 0 号帧缓冲 0x502，随后崩）：
//   主线程（上下文 A + 窗口表面）与线程 B（共享 A 的上下文 B、不绑表面）用信号量严格交替，同画一个 64×64 帧缓冲对象，各用自己上下文的 0 号顶点数组；
//   期望按 EGL / GLES 规范：B 绑定后主线程的当前上下文仍是 A、帧缓冲绑定仍是自己绑的、错误码不串；B 的 0 号帧缓冲不存在 → 状态 GL_FRAMEBUFFER_UNDEFINED（0x8219）、
//   清除 INVALID_FRAMEBUFFER_OPERATION（0x506）；A / B 各用自己的 0 号顶点数组：主线程第二笔（首顶点 6）画左第二格红、B 第二笔（不改任何状态、首顶点 6）画右第二格绿，
//   四格 x = 8 / 24 / 40 / 56 读回 红 红 绿 绿；B 删除自己的当前上下文后仍是当前（规范：松开才放），eglReleaseThread 后当前为空、B 建在 A 名下的纹理主线程照样能用（共享组）。
// N 上下文 API 与范围映射（11-03 火影忍者 终极风暴羁绊：Scaleform 按 sscanf("OpenGL ES %d.%d") 取版本，读到桌面串得 0.0、不取核心函数，渲染目标落到 0 号帧缓冲后崩；
//   补上版本串后它按 ES 3.x 取 glMapBufferRange，查表回空又往 0 写崩）。期望按 GLES 3.2 §6.1.6 / GL 4.5 §22.2 与 §6.3：
//   ES 上下文 VERSION 以「OpenGL ES 」开头、sscanf("OpenGL ES %d.%d") 读出 3 / 2，GLSL 版本以「OpenGL ES GLSL ES 」开头，MAJOR / MINOR = 3 / 2；
//   桌面上下文（eglBindAPI(EGL_OPENGL_API)）同一 sscanf 读出 0 项、MAJOR / MINOR = 4 / 5；RGBA8 表面 BLUE / ALPHA_BITS = 8，UNIFORM_BUFFER_OFFSET_ALIGNMENT = 256。
//   范围映射：偏移 8 字节、长 8 字节的只读映射读到 left[2] / left[3]；可写映射同 G 组（前一笔画旧内容左红、改写后的一笔画新内容右绿）；
//   长 0 / 越界 → INVALID_VALUE，读 + 作废 / 读写位都没有 / 显式刷新不带写 / 已映射 → INVALID_OPERATION；显式刷新：映射内无错、越过映射长 → INVALID_VALUE、映射没带显式刷新位 → INVALID_OPERATION。
// O GLSL ES 3.00（11-03 火影羁绊：Scaleform 着色器全是「#version 300 es」，出包 gl_shaders.py 交给 glslang 前没升版本，glslang -V 不收 ES 3.10 以下，421 支全翻不出）：
//   api_es300.* 与 tri.* 同一份程序、只是 #version 300 es；出库后能链接，四边形盖住左半画 (1, 0, 0) → 左 255 0 0 255、右保持清除色蓝 0 0 255 255。
// P 采样器数组（11-03 火影羁绊：Scaleform 129 段片元着色器用 uniform sampler2D tex[2]，GL 库表口径 1 没有数组项、宿主一个名字只认一个槽，全部进不了库）：
//   api_samparr.frag 取 tex[0] 画左半、tex[1] 画右半；单元 3 绑纯红 1×1、单元 5 绑纯绿 1×1，glUniform1iv(「tex」, 2, {3, 5}) →
//   左 255 0 0 255、右 0 255 0 255；「tex[1]」位置 = 「tex」位置 + 1，单设它为 3 后右半变红；活动 uniform 里数组算一个，名字「tex[0]」、大小 2。
// Q glGetShaderiv（GLES 3.2 §7.13 表 20.13 的 5 个 pname；11-03 火影羁绊运行时查 DELETE_STATUS 0x8B80 报「没实现」）：
//   tri.vert 编过 → SHADER_TYPE = VERTEX_SHADER 0x8B31、DELETE_STATUS 0、COMPILE_STATUS 1、INFO_LOG_LENGTH 0、SHADER_SOURCE_LENGTH = 文件字节数 + 1；
//   不在库里的源码 → COMPILE_STATUS 0、INFO_LOG_LENGTH = 「S2Arm: shader not in the offline library」40 字 + 1 = 41、glGetShaderInfoLog 回长 40；没给源码 → SOURCE_LENGTH 0；
//   挂在程序上时删 → 名字仍有效、DELETE_STATUS 1，分离后才真删（glIsShader 假）；未知 pname → INVALID_ENUM。
// R LÖVE 引擎的桌面上下文（11-10 Balatro iPhone 启动即闪退：建上下文后第一笔 glVertexAttrib4fv 查表回空，跳到地址 0）。桌面上下文共享 ES 上下文的程序，按 LÖVE 11.5
//   OpenGL::initMaxValues / setupContext 与流式缓冲、画四边形批的顺序调。期望按 GL 4.5 规范与 Metal 上限：
//   SAMPLE_BUFFERS 0 / SAMPLES 0（单采样表面）、CONTEXT_PROFILE_MASK 1（core）、MAX_3D_TEXTURE_SIZE 2048、MAX_ARRAY_TEXTURE_LAYERS 2048、POINT_SIZE_RANGE [1, 511]，都不置错；
//   glVertexAttrib4f 设关掉数组的 aCol = 绿 → 左半画成绿；不可变存储 + 持久一致映射（LÖVE 4.4 以上的流式缓冲）：映射着画第一段（左半红）、不解除映射接着写第二段再画（右半绿），
//   都不报错、两半都对；不可变存储上 BufferData / 没有 DYNAMIC_STORAGE 的 SubData / 可变存储上持久映射 → INVALID_OPERATION；
//   glDrawElementsBaseVertex 索引 0..5、基准顶点 6 → 画第二段：左半保持清除色蓝、右半绿；glClearBufferfv(COLOR, 0, 黄) → 黄，且 COLOR_CLEAR_VALUE 仍是设过的蓝；
//   glGetUniformfv 读回 glUniform4f 写的 (0.25, 0.5, 0.75, 1)；glPointSize(2) → POINT_SIZE 2、0 → INVALID_VALUE；glHint / glInvalidateFramebuffer 不置错、坏目标 INVALID_ENUM；
//   glDrawArraysInstanced 1 个实例同 glDrawArrays（左半红）。
// T 像素打包缓冲（D-WR8B Worms W.M.D：进战役时绑 GL_PIXEL_PACK_BUFFER → glBufferStorage → glMapBufferRange 拿指针、两条线程往里写，
//   原先三步都报「目标没实现」回空，线程写地址 0 / 0x800 崩）。期望按 GL 4.5 §6 / §18.2：绑定、建存储、查 PIXEL_PACK_BUFFER_BINDING = 绑的名字；
//   绑着时 glReadPixels 的 data 是缓冲内偏移：清除成绿后读 4×4 到偏移 16，映射偏移 16 起 64 字节读回 16 个像素都是 0 255 0 255、偏移 0..15 保持建存储时的 0xAB；
//   放不下（偏移 + 64 > 大小）、缓冲映射着（非持久）→ INVALID_OPERATION；解绑后 glReadPixels 照旧写客户端内存（读到绿）
// U 像素解包缓冲（D-W2MJ 白日梦的构想图，#1440：绑 GL_PIXEL_UNPACK_BUFFER → glMapBufferRange 写像素 → 从缓冲上传纹理，原先绑定 / 映射都报「目标没实现」、
//   纹理没上、整屏黑）。期望按 GL 4.5 §8.4.4.1 / ES 3.0 §3.7.1：绑定无错、PIXEL_UNPACK_BUFFER_BINDING = 绑的名字；96 字节缓冲偏移 0..15 填 0xAB、16..79 填 16 个红像素，
//   glTexImage2D(4×4 RGBA8, data = 偏移 16) 后纹理当附件读回 (1,1)、(3,3) 都是红；映射偏移 80 起 16 字节写 4 个绿像素，glTexSubImage2D(2×2 于 (0,0), data = 偏移 80) 后
//   (0,0)、(1,1) 绿、(3,3) 仍红；偏移 40 上传 4×4（40 + 64 > 96）、缓冲映射着（非持久）上传 → INVALID_OPERATION 且纹理不变；解绑后 glTexSubImage2D 照旧读客户端内存（(0,0) 变蓝）
#include "../../gfx/gl/gl_int.h"
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include "t_gl_stubs.inc"

typedef void *(*gpa_t)(const char *);
static gpa_t gpa = gl_lookup;
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("  过 " __VA_ARGS__); else { printf("  败 " __VA_ARGS__); fails++; } printf("\n"); } while (0)
// 本组入口：取不到 = 记一项败、本组跳过
#define G(name, ret, ...) ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))gpa(#name); if (!name) { checks++; fails++; printf("  败 取不到 %s（本组跳过）\n", #name); return; }

// ---- 已有入口（补齐前就有；取不到直接整体失败） ----
#define BASE(X) X(glGenBuffers, void, GLsizei, GLuint *) X(glBindBuffer, void, GLenum, GLuint) X(glBufferData, void, GLenum, GLsizeiptr, const void *, GLenum) \
    X(glBufferSubData, void, GLenum, GLintptr, GLsizeiptr, const void *) X(glGenVertexArrays, void, GLsizei, GLuint *) X(glBindVertexArray, void, GLuint) \
    X(glVertexAttribPointer, void, GLuint, GLint, GLenum, GLuint, GLsizei, const void *) X(glEnableVertexAttribArray, void, GLuint) X(glDisableVertexAttribArray, void, GLuint) \
    X(glCreateShader, GLuint, GLenum) X(glShaderSource, void, GLuint, GLsizei, const GLchar *const *, const GLint *) X(glCompileShader, void, GLuint) \
    X(glGetShaderiv, void, GLuint, GLenum, GLint *) X(glCreateProgram, GLuint, void) X(glAttachShader, void, GLuint, GLuint) X(glLinkProgram, void, GLuint) \
    X(glGetProgramiv, void, GLuint, GLenum, GLint *) X(glUseProgram, void, GLuint) X(glGetAttribLocation, GLint, GLuint, const GLchar *) \
    X(glGetUniformLocation, GLint, GLuint, const GLchar *) X(glUniform1i, void, GLint, GLint) X(glUniform1f, void, GLint, GLfloat) X(glUniform2f, void, GLint, GLfloat, GLfloat) \
    X(glUniform4f, void, GLint, GLfloat, GLfloat, GLfloat, GLfloat) X(glClearColor, void, GLfloat, GLfloat, GLfloat, GLfloat) X(glClear, void, GLbitfield) \
    X(glClearDepthf, void, GLfloat) X(glEnable, void, GLenum) X(glDisable, void, GLenum) X(glScissor, void, GLint, GLint, GLsizei, GLsizei) \
    X(glViewport, void, GLint, GLint, GLsizei, GLsizei) X(glDrawArrays, void, GLenum, GLint, GLsizei) X(glDrawElements, void, GLenum, GLsizei, GLenum, const void *) \
    X(glFlush, void, void) X(glFinish, void, void) X(glGetError, GLenum, void) X(glGetIntegerv, void, GLenum, GLint *) X(glGetFloatv, void, GLenum, GLfloat *) \
    X(glGenFramebuffers, void, GLsizei, GLuint *) X(glBindFramebuffer, void, GLenum, GLuint) X(glDeleteFramebuffers, void, GLsizei, const GLuint *) \
    X(glFramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint) X(glFramebufferRenderbuffer, void, GLenum, GLenum, GLenum, GLuint) \
    X(glCheckFramebufferStatus, GLenum, GLenum) X(glGenRenderbuffers, void, GLsizei, GLuint *) X(glBindRenderbuffer, void, GLenum, GLuint) \
    X(glDeleteRenderbuffers, void, GLsizei, const GLuint *) X(glRenderbufferStorage, void, GLenum, GLenum, GLsizei, GLsizei) \
    X(glGetRenderbufferParameteriv, void, GLenum, GLenum, GLint *) X(glBlitFramebuffer, void, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, uint64_t, uint64_t) \
    X(glReadPixels, void, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) X(glDrawBuffers, void, GLsizei, const GLenum *) \
    X(glGenTextures, void, GLsizei, GLuint *) X(glBindTexture, void, GLenum, GLuint) X(glActiveTexture, void, GLenum) X(glDeleteTextures, void, GLsizei, const GLuint *) \
    X(glTexImage2D, void, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) X(glTexParameteri, void, GLenum, GLenum, GLint) \
    X(glCompressedTexImage2D, void, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *) X(glPixelStorei, void, GLenum, GLint) \
    X(glDeleteBuffers, void, GLsizei, const GLuint *) X(glDeleteShader, void, GLuint) X(glDeleteProgram, void, GLuint) X(glBindAttribLocation, void, GLuint, GLuint, const GLchar *) \
    X(eglSwapBuffers, EGLBoolean, EGLDisplay, EGLSurface)
#define DECL(name, ret, ...) static ret (*name)(__VA_ARGS__);
BASE(DECL)
#undef DECL
static EGLDisplay g_dpy; static EGLSurface g_sf;

static int near(int a, int b) { return a - b <= 1 && b - a <= 1; }
static void rpx(int x, int y, uint8_t p[4]) { memset(p, 0, 4); glReadPixels(x, y, 1, 1, 0x1908, 0x1401, p); }   // 当前读帧缓冲第 y 行第 x 列（GL 行序）
static int px_is(int x, int y, int r, int g, int b, int a) { uint8_t p[4]; rpx(x, y, p); return near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a); }
static void pxs(int x, int y, char *out) { uint8_t p[4]; rpx(x, y, p); sprintf(out, "%u %u %u %u", p[0], p[1], p[2], p[3]); }
static char *slurp(const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", getenv("T_GL_DIR"), name); FILE *f = fopen(path, "rb"); if (!f) { printf("✗ 读不到 %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *s = calloc(1, (size_t)n + 1); if (fread(s, 1, (size_t)n, f) != (size_t)n) exit(1); fclose(f); return s;
}
static GLuint shader(GLenum type, const char *file) {
    char *src = slurp(file); GLuint s = glCreateShader(type); const GLchar *t = src; glShaderSource(s, 1, &t, NULL); free(src); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, 0x8B81, &ok); if (!ok) { printf("✗ 着色器 %s 不在库里\n", file); exit(1); } return s;
}
static GLuint program(const char *name) {
    char v[64], f[64]; snprintf(v, sizeof v, "%s.vert", name); snprintf(f, sizeof f, "%s.frag", name);
    GLuint p = glCreateProgram(); glAttachShader(p, shader(0x8B31, v)); glAttachShader(p, shader(0x8B30, f)); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, 0x8B82, &ok); if (!ok) { printf("✗ 程序 %s 链接失败\n", name); exit(1); } return p;
}
// 64×64（或给定尺寸）RGBA8 渲染缓冲帧缓冲，绑为当前读写帧缓冲
static GLuint fbo_rb(int w, int h, GLuint *rb_out) {
    GLuint fb, rb; glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb); glGenRenderbuffers(1, &rb); glBindRenderbuffer(0x8D41, rb);
    glRenderbufferStorage(0x8D41, 0x8058, w, h); glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb); glViewport(0, 0, w, h);
    if (rb_out) *rb_out = rb; return fb;
}
// tri 程序（aPos vec2 + aCol vec3，uOff、uTint）的顶点数组：n 个顶点，每顶点 x y r g b
static GLuint g_tri, g_tri_vao, g_tri_vbo;
static void tri_use(const float *v, int n) {
    glUseProgram(g_tri); glBindVertexArray(g_tri_vao); glBindBuffer(0x8892, g_tri_vbo); glBufferData(0x8892, (GLsizeiptr)(n * 5 * sizeof(float)), v, 0x88E4);
    GLint p = glGetAttribLocation(g_tri, "aPos"), c = glGetAttribLocation(g_tri, "aCol");
    glVertexAttribPointer((GLuint)p, 2, 0x1406, 0, 20, (void *)0); glEnableVertexAttribArray((GLuint)p);
    glVertexAttribPointer((GLuint)c, 3, 0x1406, 0, 20, (void *)8); glEnableVertexAttribArray((GLuint)c);
    glUniform2f(glGetUniformLocation(g_tri, "uOff"), 0, 0); glUniform4f(glGetUniformLocation(g_tri, "uTint"), 1, 1, 1, 1);
}
#define QUAD(x0, y0, x1, y1, r, g, b) x0, y0, r, g, b,  x1, y0, r, g, b,  x0, y1, r, g, b,  x1, y0, r, g, b,  x1, y1, r, g, b,  x0, y1, r, g, b
enum { INVALID_ENUM = 0x0500, INVALID_VALUE = 0x0501, INVALID_OPERATION = 0x0502 };

// ---------- A 查询对象 ----------
static void t_queries(void) {
    printf("A 查询对象\n");
    G(glGenQueries, void, GLsizei, GLuint *) G(glDeleteQueries, void, GLsizei, const GLuint *) G(glBeginQuery, void, GLenum, GLuint) G(glEndQuery, void, GLenum)
    G(glGetQueryObjectiv, void, GLuint, GLenum, GLint *) G(glGetQueryObjectivARB, void, GLuint, GLenum, GLint *) G(glGetQueryObjectuiv, void, GLuint, GLenum, GLuint *)
    GLuint fb = fbo_rb(64, 64, NULL); const float left[] = {QUAD(-1, -1, 0, 1, 1, 1, 1)}; tri_use(left, 6);
    glClearColor(0, 0, 0, 1); glClear(0x4000);
    GLuint q[4]; glGenQueries(4, q); GLuint r = 0; GLint ri = 0;
    glBeginQuery(0x8914, q[0]); glDrawArrays(4, 0, 6); glEndQuery(0x8914);
    glGetQueryObjectuiv(q[0], 0x8866, &r); CHECK(r == 2048 && !glGetError(), "SAMPLES_PASSED：左半四边形 32×64 = 2048 样本（实测 %u）", r);
    glGetQueryObjectiv(q[0], 0x8866, &ri); GLint ra = 0; glGetQueryObjectivARB(q[0], 0x8866, &ra); CHECK(ri == 2048 && ra == 2048, "iv / ivARB 读同一结果（%d / %d）", ri, ra);
    glBeginQuery(0x8C2F, q[1]); glEndQuery(0x8C2F); r = 9; glGetQueryObjectuiv(q[1], 0x8866, &r); CHECK(r == 0, "ANY_SAMPLES_PASSED：期间没画 = 0（实测 %u）", r);
    glBeginQuery(0x8D6A, q[1]); CHECK(glGetError() == INVALID_OPERATION, "查询对象换目标再开始 → INVALID_OPERATION");
    glBeginQuery(0x8C2F, q[1]); glDrawArrays(4, 0, 6); glEndQuery(0x8C2F); glGetQueryObjectuiv(q[1], 0x8866, &r); CHECK(r == 1, "ANY_SAMPLES_PASSED：画了 = 1（实测 %u）", r);
    // 跨提交 + 局部清除：两次绘制各 2048、中间 glFlush 换命令缓冲、裁剪清除走清除绘制（不计）
    glBeginQuery(0x8914, q[2]); glDrawArrays(4, 0, 6); glFlush();
    glEnable(0x0C11); glScissor(0, 0, 10, 10); glClear(0x4000); glDisable(0x0C11); glDrawArrays(4, 0, 6); glEndQuery(0x8914);
    glGetQueryObjectuiv(q[2], 0x8866, &r); CHECK(r == 4096, "跨 glFlush 两笔 + 局部清除：4096 样本（清除不计，实测 %u）", r);
    // 错误：同目标重复开始、活动中取结果、没活动时结束
    glBeginQuery(0x8914, q[0]); glBeginQuery(0x8914, q[3]); GLenum e1 = glGetError(); glGetQueryObjectuiv(q[0], 0x8866, &r); GLenum e2 = glGetError();
    glEndQuery(0x8914); glEndQuery(0x8914); GLenum e3 = glGetError();
    CHECK(e1 == INVALID_OPERATION && e2 == INVALID_OPERATION && e3 == INVALID_OPERATION, "同目标已活动再开始 / 活动中取结果 / 没活动时结束 → INVALID_OPERATION（%#x %#x %#x）", e1, e2, e3);
    glBeginQuery(0x1234, q[3]); CHECK(glGetError() == INVALID_ENUM, "未知目标 → INVALID_ENUM");
    // 可取性：结束后反复查 AVAILABLE 最终为真（实现须自行刷新）
    glBeginQuery(0x8914, q[3]); glDrawArrays(4, 0, 6); glEndQuery(0x8914); GLuint av = 0;
    for (int i = 0; i < 2000 && !av; i++) { glGetQueryObjectuiv(q[3], 0x8867, &av); if (!av) usleep(1000); }
    glGetQueryObjectuiv(q[3], 0x8866, &r); CHECK(av == 1 && r == 2048, "QUERY_RESULT_AVAILABLE 最终为 1、结果 2048（%u / %u）", av, r);
    // 经过时间 / 生成图元数
    glBeginQuery(0x88BF, q[0]); for (int i = 0; i < 10; i++) glDrawArrays(4, 0, 6); glEndQuery(0x88BF);
    GLuint ns = 0; glGetQueryObjectuiv(q[0], 0x8866, &ns);
    CHECK(glGetError() == INVALID_OPERATION && !glGetError(), "SAMPLES_PASSED 用过的对象换成 TIME_ELAPSED → INVALID_OPERATION");
    GLuint qt; glGenQueries(1, &qt); glBeginQuery(0x88BF, qt); for (int i = 0; i < 10; i++) glDrawArrays(4, 0, 6); glEndQuery(0x88BF);
    glGetQueryObjectuiv(qt, 0x8866, &ns); CHECK(ns > 0 && ns < 1000000000u && !glGetError(), "TIME_ELAPSED：10 笔绘制 GPU 用时 %u ns（> 0、< 1 秒）", ns);
    GLuint qp; glGenQueries(1, &qp); glBeginQuery(0x8C87, qp); glDrawArrays(4, 0, 6); glEndQuery(0x8C87); glGetQueryObjectuiv(qp, 0x8866, &r);
    CHECK(r == 2, "PRIMITIVES_GENERATED：6 顶点 TRIANGLES = 2（实测 %u）", r);
    glDeleteQueries(4, q); glDeleteQueries(1, &qt); glDeleteQueries(1, &qp); glGetQueryObjectuiv(q[0], 0x8866, &r); CHECK(glGetError() == INVALID_OPERATION, "删掉的查询取结果 → INVALID_OPERATION");
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

// ---------- B 同步对象 ----------
static void t_sync(void) {
    printf("B 同步对象\n");
    G(glFenceSync, GLsync, GLenum, GLbitfield) G(glClientWaitSync, GLenum, GLsync, GLbitfield, GLuint64) G(glWaitSync, void, GLsync, GLbitfield, GLuint64) G(glDeleteSync, void, GLsync)
    glBindFramebuffer(0x8D40, 0); glViewport(0, 0, 1920, 1080); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float quad[] = {QUAD(-1, -1, 1, 1, 1, 1, 0)}; tri_use(quad, 6); glDrawArrays(4, 0, 6);
    id<MTLTexture> col = gl_surface_color();
    GLsync f = glFenceSync(0x9117, 0); GLenum r = glClientWaitSync(f, 1, 5000000000ull);
    uint8_t px[4]; { id<MTLDevice> d = (__bridge id<MTLDevice>)mtl_device(); id<MTLBuffer> b = [d newBufferWithLength:4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:col sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(500, 500, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted]; memcpy(px, b.contents, 4); }
    CHECK((r == 0x911C || r == 0x911A) && px[0] == 255 && px[1] == 255 && px[2] == 0, "ClientWaitSync 等到（%#x），另一队列读到栅栏前画的黄 %u %u %u", r, px[0], px[1], px[2]);
    CHECK(glClientWaitSync(f, 0, 0) == 0x911A, "再等 → ALREADY_SIGNALED");
    CHECK(glClientWaitSync(f, 2, 0) == 0x911D && glGetError() == INVALID_VALUE, "未知 flags → WAIT_FAILED + INVALID_VALUE");
    glWaitSync(f, 0, 0xFFFFFFFFFFFFFFFFull); GLenum e1 = glGetError(); glWaitSync(f, 0, 5); GLenum e2 = glGetError();
    CHECK(e1 == 0 && e2 == INVALID_VALUE, "WaitSync：GL_TIMEOUT_IGNORED 收、别的超时 INVALID_VALUE（%#x %#x）", e1, e2);
    glDrawArrays(4, 0, 6); GLsync g = glFenceSync(0x9117, 0); GLenum r0 = glClientWaitSync(g, 0, 0), r1 = glClientWaitSync(g, 0, 5000000000ull);
    CHECK((r0 == 0x911B || r0 == 0x911A) && (r1 == 0x911C || r1 == 0x911A) && !glGetError(), "超时 0 只查不等（%#x），再按 5 秒等到（%#x）", r0, r1);
    glDeleteSync(f); GLenum e3 = glGetError(); glDeleteSync(f); GLenum e4 = glGetError(); glDeleteSync(NULL); GLenum e5 = glGetError(); glDeleteSync(g);
    CHECK(e3 == 0 && e4 == INVALID_VALUE && e5 == 0, "DeleteSync：删一次收、再删 INVALID_VALUE、0 静默（%#x %#x %#x）", e3, e4, e5);
    CHECK(glFenceSync(0x1234, 0) == NULL && glGetError() == INVALID_ENUM, "FenceSync 未知条件 → INVALID_ENUM、回 0");
    CHECK(glClientWaitSync(f, 0, 0) == 0x911D && glGetError() == INVALID_VALUE, "等已删的栅栏 → WAIT_FAILED + INVALID_VALUE");
}

// ---------- C 读回 ----------
static void t_readback(void) {
    printf("C 纹理读回\n");
    G(glGetTexImage, void, GLenum, GLint, GLenum, GLenum, void *) G(glGetCompressedTexImage, void, GLenum, GLint, void *)
    GLuint t[6]; glGenTextures(6, t); glActiveTexture(0x84C0); glPixelStorei(0x0CF5, 1);
    const uint8_t rgba[16] = {1, 2, 3, 4, 50, 60, 70, 80, 200, 201, 202, 203, 255, 0, 128, 7}; uint8_t out[64];
    glBindTexture(0x0DE1, t[0]); glTexImage2D(0x0DE1, 0, 0x8058, 2, 2, 0, 0x1908, 0x1401, rgba);
    memset(out, 0xEE, sizeof out); glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, out); CHECK(!memcmp(out, rgba, 16) && !glGetError(), "RGBA8 2×2 原样读回");
    const uint8_t r8[6] = {10, 20, 30, 40, 50, 60};
    glBindTexture(0x0DE1, t[1]); glTexImage2D(0x0DE1, 0, 0x8229, 3, 2, 0, 0x1903, 0x1401, r8);
    memset(out, 0xEE, sizeof out); glPixelStorei(0x0D05, 4); glGetTexImage(0x0DE1, 0, 0x1903, 0x1401, out);
    CHECK(out[0] == 10 && out[2] == 30 && out[3] == 0xEE && out[4] == 40 && out[6] == 60, "R8 宽 3、PACK_ALIGNMENT 4：每行 4 字节、填充字节不写（%u %u %u | %u %u）", out[0], out[2], out[3], out[4], out[6]);
    glPixelStorei(0x0D05, 1); glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, out);
    CHECK(out[0] == 10 && out[1] == 0 && out[2] == 0 && out[3] == 255 && out[4] == 20, "R8 按 RGBA 读：(r, 0, 0, 255)（%u %u %u %u）", out[0], out[1], out[2], out[3]);
    _Float16 h4[4] = {0.25, 0.5, 2.0, 1.0}; _Float16 hb[4]; float fb4[4];
    glBindTexture(0x0DE1, t[2]); glTexImage2D(0x0DE1, 0, 0x881A, 1, 1, 0, 0x1908, 0x140B, h4);
    glGetTexImage(0x0DE1, 0, 0x1908, 0x140B, hb); glGetTexImage(0x0DE1, 0, 0x1908, 0x1406, fb4);
    CHECK(!memcmp(hb, h4, 8) && fb4[0] == 0.25f && fb4[2] == 2.0f, "RGBA16F：半精度位精确、按浮点读 %g %g %g %g", fb4[0], fb4[1], fb4[2], fb4[3]);
    // GPU 写过的：清成绿色后（没 glFinish）读回
    GLuint fb; glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb);
    glBindTexture(0x0DE1, t[3]); glTexImage2D(0x0DE1, 0, 0x8058, 4, 4, 0, 0x1908, 0x1401, NULL); glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, t[3], 0);
    glViewport(0, 0, 4, 4); glClearColor(0, 1, 0, 1); glClear(0x4000);
    glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, out); int green = 1; for (int i = 0; i < 16; i++) green &= out[4 * i] == 0 && out[4 * i + 1] == 255 && out[4 * i + 3] == 255;
    CHECK(green, "GPU 清除写的颜色：读回前等 GPU，16 像素全绿");
    // 私有存储的深度：清 0.25 后按浮点读回
    glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, 0, 0);
    glBindTexture(0x0DE1, t[4]); glTexImage2D(0x0DE1, 0, 0x8CAC, 4, 4, 0, 0x1902, 0x1406, NULL); glFramebufferTexture2D(0x8D40, 0x8D00, 0x0DE1, t[4], 0);
    glClearDepthf(0.25f); glClear(0x100); float dep[16] = {0};
    glGetTexImage(0x0DE1, 0, 0x1902, 0x1406, dep); int ok = 1; for (int i = 0; i < 16; i++) ok &= dep[i] == 0.25f;
    CHECK(ok && !glGetError(), "深度 32F（私有存储）清 0.25 后读回 16 个 0.25（首个 %g）", dep[0]);
    glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, out); CHECK(glGetError() == INVALID_OPERATION, "深度纹理按颜色读 → INVALID_OPERATION");
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
    // 压缩：BC1 一块原样读回；非压缩用压缩读回 / 压缩用非压缩读回都报错
    const uint8_t bc1[8] = {0x00, 0xF8, 0xE0, 0x07, 0x1B, 0x6C, 0xB1, 0xC6}; uint8_t cb[8] = {0};
    glBindTexture(0x0DE1, t[5]); glCompressedTexImage2D(0x0DE1, 0, 0x83F1, 4, 4, 0, 8, bc1);
    glGetCompressedTexImage(0x0DE1, 0, cb); CHECK(!memcmp(cb, bc1, 8) && !glGetError(), "BC1 4×4 一块 8 字节原样读回");
    glGetTexImage(0x0DE1, 0, 0x1908, 0x1401, out); GLenum e1 = glGetError();
    glBindTexture(0x0DE1, t[0]); glGetCompressedTexImage(0x0DE1, 0, cb); GLenum e2 = glGetError();
    CHECK(e1 == INVALID_OPERATION && e2 == INVALID_OPERATION, "压缩纹理非压缩读 / 非压缩纹理压缩读 → INVALID_OPERATION（%#x %#x）", e1, e2);
    GLuint cube; glGenTextures(1, &cube); glBindTexture(0x8513, cube); const uint8_t face[4] = {9, 8, 7, 6};
    glTexImage2D(0x8517, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, face); memset(out, 0, 4); glGetTexImage(0x8517, 0, 0x1908, 0x1401, out);
    CHECK(!memcmp(out, face, 4) && !glGetError(), "立方体 +Y 面原样读回");
    glGetTexImage(0x8513, 0, 0x1908, 0x1401, out); CHECK(glGetError() == INVALID_ENUM, "GL_TEXTURE_CUBE_MAP 本身不是读回目标 → INVALID_ENUM");
    glDeleteTextures(6, t); glDeleteTextures(1, &cube); glPixelStorei(0x0D05, 4); glPixelStorei(0x0CF5, 4);
}

// ---------- D 帧缓冲附件查询 ----------
static void t_fb_query(void) {
    printf("D 帧缓冲附件查询\n");
    G(glGetFramebufferAttachmentParameteriv, void, GLenum, GLenum, GLenum, GLint *)
    GLuint fb, tex, rb, rb3; glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb); glGenTextures(1, &tex); glBindTexture(0x0DE1, tex);
    glTexImage2D(0x0DE1, 0, 0x8058, 8, 8, 0, 0x1908, 0x1401, NULL); glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(0x8D41, rb); glRenderbufferStorage(0x8D41, 0x88F0, 8, 8); glFramebufferRenderbuffer(0x8D40, 0x821A, 0x8D41, rb);
    GLint v[12]; for (int i = 0; i < 12; i++) v[i] = -1;
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD0, &v[0]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD1, &v[1]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD2, &v[2]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8212, &v[3]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8215, &v[4]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8211, &v[5]);
    CHECK(v[0] == 0x1702 && v[1] == (GLint)tex && v[2] == 0 && v[3] == 8 && v[4] == 8 && v[5] == 0x8C17 && !glGetError(), "RGBA8 纹理附件：类型 TEXTURE、名字 %d、级 %d、红 %d、alpha %d、UNSIGNED_NORMALIZED", v[1], v[2], v[3], v[4]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x821A, 0x8CD0, &v[0]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x821A, 0x8216, &v[1]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x821A, 0x8217, &v[2]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x8D00, 0x8211, &v[3]);
    CHECK(v[0] == 0x8D41 && v[1] == 32 && v[2] == 8 && v[3] == 0x1406 && !glGetError(), "深度模板渲染缓冲：RENDERBUFFER、深度 %d 位（存 32 位浮点）、模板 %d、深度分量 FLOAT", v[1], v[2]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x821A, 0x8211, &v[0]); CHECK(glGetError() == INVALID_OPERATION, "DEPTH_STENCIL 查分量类型 → INVALID_OPERATION");
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE1, 0x8CD0, &v[0]); GLint none = v[0]; glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE1, 0x8212, &v[1]);
    CHECK(none == 0 && glGetError() == INVALID_OPERATION, "没挂的 COLOR_ATTACHMENT1：类型 NONE，查位数 → INVALID_OPERATION");
    glGenRenderbuffers(1, &rb3); glBindRenderbuffer(0x8D41, rb3); glRenderbufferStorage(0x8D41, 0x8051, 8, 8); glFramebufferRenderbuffer(0x8D40, 0x8CE2, 0x8D41, rb3);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE2, 0x8215, &v[0]); CHECK(v[0] == 0, "RGB8 渲染缓冲：alpha 0 位（实测 %d）", v[0]);
    glBindFramebuffer(0x8D40, 0);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x0405, 0x8CD0, &v[0]); glGetFramebufferAttachmentParameteriv(0x8D40, 0x0405, 0x8214, &v[1]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x1801, 0x8216, &v[2]);
    CHECK(v[0] == 0x8218 && v[1] == 8 && v[2] == 32 && !glGetError(), "0 号帧缓冲：BACK 类型 FRAMEBUFFER_DEFAULT、蓝 %d 位、DEPTH %d 位", v[1], v[2]);
    glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD0, &v[0]); CHECK(glGetError() == INVALID_OPERATION, "0 号帧缓冲查 COLOR_ATTACHMENT0 → INVALID_OPERATION");
    glDeleteFramebuffers(1, &fb); glDeleteTextures(1, &tex); glDeleteRenderbuffers(1, &rb); glDeleteRenderbuffers(1, &rb3);
}

// ---------- E 体纹理 ----------
static void t_volume(void) {
    printf("E 体纹理（2D 数组 / 3D）\n");
    G(glTexImage3D, void, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, uint64_t, const void *)   // 栈上参数按游戏的 8 字节一格
    G(glTexSubImage3D, void, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, uint64_t, uint64_t, const void *)   // 栈上参数按游戏的 8 字节一格（gl_tex.m 注释）
    G(glGetTexImage, void, GLenum, GLint, GLenum, GLenum, void *)
    GLuint prog = program("api_vol"), fb = fbo_rb(8, 8, NULL); glUseProgram(prog);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}; glBufferData(0x8892, sizeof quad, quad, 0x88E4);
    GLint ap = glGetAttribLocation(prog, "aPos"); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 8, 0); glEnableVertexAttribArray((GLuint)ap);
    uint8_t arr[3][4][4], vol[4][4][4]; const uint8_t lc[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    for (int l = 0; l < 3; l++) for (int i = 0; i < 4; i++) memcpy(arr[l][i], lc[l], 4);
    for (int z = 0; z < 4; z++) for (int i = 0; i < 4; i++) { vol[z][i][0] = (uint8_t)(40 + 60 * z); vol[z][i][1] = 7; vol[z][i][2] = 0; vol[z][i][3] = 255; }
    GLuint t[2]; glGenTextures(2, t);
    glActiveTexture(0x84C0); glBindTexture(0x8C1A, t[0]); glTexImage3D(0x8C1A, 0, 0x8058, 2, 2, 3, 0, 0x1908, 0x1401, arr);
    glTexParameteri(0x8C1A, 0x2801, 0x2600); glTexParameteri(0x8C1A, 0x2800, 0x2600);
    glActiveTexture(0x84C1); glBindTexture(0x806F, t[1]); glTexImage3D(0x806F, 0, 0x8058, 2, 2, 4, 0, 0x1908, 0x1401, vol);
    glTexParameteri(0x806F, 0x2801, 0x2600); glTexParameteri(0x806F, 0x2800, 0x2600);
    CHECK(!glGetError(), "TexImage3D：2D 数组 2×2×3、3D 2×2×4 定义无错");
    glUniform1i(glGetUniformLocation(prog, "uArr"), 0); glUniform1i(glGetUniformLocation(prog, "uVol"), 1);
    GLint sel = glGetUniformLocation(prog, "uSel"), uz = glGetUniformLocation(prog, "uZ"); char s[32];
    for (int l = 0; l < 3; l++) {
        glUniform1i(sel, 0); glUniform1f(uz, (float)l); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
        CHECK(px_is(4, 4, lc[l][0], lc[l][1], lc[l][2], 255), "2D 数组第 %d 层采样 = 该层纯色（%s）", l, s);
    }
    for (int z = 0; z < 4; z++) {
        glUniform1i(sel, 1); glUniform1f(uz, (z + 0.5f) / 4); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
        CHECK(px_is(4, 4, 40 + 60 * z, 7, 0, 255), "3D 第 %d 片中心采样 = %d 7 0（%s）", z, 40 + 60 * z, s);
    }
    // 在途更新：左半先画（读旧层 1 = 绿），子图把层 1 改白，右半再画；两笔各读各的
    const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    glUniform1i(sel, 0); glUniform1f(uz, 1); glViewport(0, 0, 4, 8); glDrawArrays(4, 0, 6);
    glActiveTexture(0x84C0); glTexSubImage3D(0x8C1A, 0, 0, 0, 1, 2, 2, 1, 0x1908, 0x1401, white);
    glViewport(4, 0, 4, 8); glDrawArrays(4, 0, 6); glFinish(); glViewport(0, 0, 8, 8);
    CHECK(px_is(1, 4, 0, 255, 0, 255) && px_is(6, 4, 255, 255, 255, 255) && !glGetError(), "TexSubImage3D 在途：之前那笔读绿、之后那笔读白");
    uint8_t back[3][4][4]; memset(back, 0, sizeof back); glPixelStorei(0x0D05, 1); glGetTexImage(0x8C1A, 0, 0x1908, 0x1401, back);
    CHECK(!memcmp(back[0], arr[0], 16) && !memcmp(back[1], white, 16) && !memcmp(back[2], arr[2], 16), "2D 数组 GetTexImage 三层字节（层 1 已改白）");
    glActiveTexture(0x84C1); const uint8_t px3[4] = {1, 2, 3, 4}; glTexSubImage3D(0x806F, 0, 1, 1, 3, 1, 1, 1, 0x1908, 0x1401, px3);
    uint8_t v3[4][4][4]; glGetTexImage(0x806F, 0, 0x1908, 0x1401, v3);
    CHECK(!memcmp(v3[3][3], px3, 4) && !memcmp(v3[3][0], vol[3][0], 4) && !memcmp(v3[0][3], vol[0][3], 4), "3D 子图写 (1,1,3) 一个像素，GetTexImage 只那一处变");
    glTexImage3D(0x0DE1, 0, 0x8058, 2, 2, 2, 0, 0x1908, 0x1401, NULL); GLenum e1 = glGetError();
    glTexImage3D(0x806F, 0, 0x8CAC, 2, 2, 2, 0, 0x1902, 0x1406, NULL); GLenum e2 = glGetError();
    glTexSubImage3D(0x806F, 0, 0, 0, 3, 2, 2, 2, 0x1908, 0x1401, vol); GLenum e3 = glGetError();
    CHECK(e1 == INVALID_ENUM && e2 == INVALID_OPERATION && e3 == INVALID_VALUE, "TexImage3D 2D 目标 INVALID_ENUM、3D 深度格式 INVALID_OPERATION、子图越界 INVALID_VALUE（%#x %#x %#x）", e1, e2, e3);
    glActiveTexture(0x84C0); glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb); glDeleteTextures(2, t); glPixelStorei(0x0D05, 4);
}

// ---------- F 多重采样渲染缓冲 + 解析 ----------
static void t_msaa(void) {
    printf("F 多重采样\n");
    G(glRenderbufferStorageMultisample, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei)
    GLuint ms, rb, rd, ss, tex; glGenFramebuffers(1, &ms); glBindFramebuffer(0x8D40, ms); glGenRenderbuffers(1, &rb); glBindRenderbuffer(0x8D41, rb);
    glRenderbufferStorageMultisample(0x8D41, 4, 0x8058, 64, 64); glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb);
    GLint n = 0; glGetRenderbufferParameteriv(0x8D41, 0x8CAB, &n);
    CHECK(n >= 4 && glCheckFramebufferStatus(0x8D40) == 0x8CD5, "4× 渲染缓冲：RENDERBUFFER_SAMPLES %d、帧缓冲完整", n);
    glViewport(0, 0, 64, 64); glClearColor(1, 0, 0, 1); glClear(0x4000);
    const float tri[] = {-1, -1, 0, 1, 0,  1, -1, 0, 1, 0,  -1, 1, 0, 1, 0}; tri_use(tri, 3); glDrawArrays(4, 0, 3);
    glGenFramebuffers(1, &ss); glBindFramebuffer(0x8CA9, ss); glGenTextures(1, &tex); glBindTexture(0x0DE1, tex);
    glTexImage2D(0x0DE1, 0, 0x8058, 64, 64, 0, 0x1908, 0x1401, NULL); glFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, tex, 0);
    glBindFramebuffer(0x8CA8, ms); glBlitFramebuffer(0, 0, 64, 64, 0, 0, 64, 64, 0x4000, 0x2600); GLenum e = glGetError();
    glBindFramebuffer(0x8D40, ss); glFinish();
    int mixed = 0; char s[32]; for (int x = 24; x < 40; x++) { uint8_t p[4]; rpx(x, 32, p); if (p[0] > 20 && p[0] < 235 && p[1] > 20 && p[1] < 235) mixed++; }
    CHECK(e == 0 && px_is(5, 5, 0, 255, 0, 255) && px_is(60, 60, 255, 0, 0, 255), "解析后：三角形内纯绿、外纯红");
    pxs(31, 32, s); CHECK(mixed > 0, "对角线穿过的像素是混色（多重采样真生效）：第 32 行 x 24–39 混色 %d 个、(31,32) = %s", mixed, s);
    glBindFramebuffer(0x8CA8, ms); glBindFramebuffer(0x8CA9, ss); glBlitFramebuffer(0, 0, 64, 64, 0, 0, 32, 32, 0x4000, 0x2600);
    CHECK(glGetError() == INVALID_OPERATION, "解析时源 / 目标矩形尺寸不同 → INVALID_OPERATION");
    glBindFramebuffer(0x8D40, ms); glGenRenderbuffers(1, &rd); glBindRenderbuffer(0x8D41, rd); glRenderbufferStorage(0x8D41, 0x81A5, 64, 64);
    glFramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, rd);
    GLenum st = glCheckFramebufferStatus(0x8D40); CHECK(st == 0x8D56, "4× 颜色 + 单采样深度：FRAMEBUFFER_INCOMPLETE_MULTISAMPLE（实测 %#x）", st);
    glRenderbufferStorageMultisample(0x8D41, 9, 0x81A5, 64, 64); CHECK(glGetError() == INVALID_OPERATION, "样本数 > MAX_SAMPLES（ES 上下文）→ INVALID_OPERATION");
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &ms); glDeleteFramebuffers(1, &ss); glDeleteRenderbuffers(1, &rb); glDeleteRenderbuffers(1, &rd); glDeleteTextures(1, &tex);
}

// ---------- G 缓冲映射 ----------
static void t_map(void) {
    printf("G 缓冲映射\n");
    G(glMapBuffer, void *, GLenum, GLenum) G(glUnmapBuffer, GLboolean, GLenum)
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float left[] = {QUAD(-1, -1, 0, 1, 1, 0, 0)}; tri_use(left, 6);
    float *ro = glMapBuffer(0x8892, 0x88B8); int same = ro && !memcmp(ro, left, sizeof left); GLboolean un = glUnmapBuffer(0x8892);
    CHECK(same && un == 1 && !glGetError(), "只读映射读到 BufferData 的内容、Unmap 回 TRUE");
    glDrawArrays(4, 0, 6);   // 这笔读旧内容（左半红）
    float *w = glMapBuffer(0x8892, 0x88B9);
    CHECK(w && glMapBuffer(0x8892, 0x88B9) == NULL && glGetError() == INVALID_OPERATION, "已映射再映射 → INVALID_OPERATION");
    glBufferSubData(0x8892, 0, 4, left); CHECK(glGetError() == INVALID_OPERATION, "映射着 BufferSubData → INVALID_OPERATION");
    glDrawArrays(4, 0, 6); CHECK(glGetError() == INVALID_OPERATION, "映射着画 → INVALID_OPERATION（不画）");
    const float right[] = {QUAD(0, -1, 1, 1, 0, 1, 0)}; if (w) memcpy(w, right, sizeof right);
    CHECK(glUnmapBuffer(0x8892) == 1, "可写映射 Unmap 回 TRUE");
    glDrawArrays(4, 0, 6); glFinish(); char a[32], b[32]; pxs(16, 32, a); pxs(48, 32, b);
    CHECK(px_is(16, 32, 255, 0, 0, 255) && px_is(48, 32, 0, 255, 0, 255), "映射前的那笔画旧内容（左红 %s）、映射写后的画新内容（右绿 %s）", a, b);
    glUnmapBuffer(0x8892); GLenum e1 = glGetError(); glMapBuffer(0x8892, 0x1234); GLenum e2 = glGetError();
    CHECK(e1 == INVALID_OPERATION && e2 == INVALID_ENUM, "没映射时 Unmap → INVALID_OPERATION、未知访问方式 → INVALID_ENUM");
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

// ---------- H 实例绘制 + 除数 + 当前通用属性 ----------
static void t_instanced(void) {
    printf("H 实例绘制\n");
    G(glDrawElementsInstanced, void, GLenum, GLsizei, GLenum, const void *, GLsizei) G(glVertexAttribDivisor, void, GLuint, GLuint) G(glVertexAttrib1f, void, GLuint, GLfloat)
    G(glBeginQuery, void, GLenum, GLuint) G(glEndQuery, void, GLenum) G(glGenQueries, void, GLsizei, GLuint *) G(glGetQueryObjectuiv, void, GLuint, GLenum, GLuint *)
    GLuint fb = fbo_rb(64, 64, NULL), prog = program("api_inst"); glUseProgram(prog); glClearColor(0, 0, 0, 1); glClear(0x4000);
    GLuint vao, b[3]; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(3, b);
    const float pos[] = {-.1f, -.1f, .1f, -.1f, .1f, .1f, -.1f, .1f}, off[] = {-.5f, 0, 0, 0, .5f, 0}; const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    GLint ap = glGetAttribLocation(prog, "aPos"), ao = glGetAttribLocation(prog, "aOff"), ak = glGetAttribLocation(prog, "aK");
    glBindBuffer(0x8892, b[0]); glBufferData(0x8892, sizeof pos, pos, 0x88E4); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 0, 0); glEnableVertexAttribArray((GLuint)ap);
    glBindBuffer(0x8892, b[1]); glBufferData(0x8892, sizeof off, off, 0x88E4); glVertexAttribPointer((GLuint)ao, 2, 0x1406, 0, 0, 0); glEnableVertexAttribArray((GLuint)ao);
    glVertexAttribDivisor((GLuint)ao, 1); glDisableVertexAttribArray((GLuint)ak); glVertexAttrib1f((GLuint)ak, 0.5f);
    glBindBuffer(0x8893, b[2]); glBufferData(0x8893, sizeof idx, idx, 0x88E4);
    GLuint q; glGenQueries(1, &q); glBeginQuery(0x8C87, q);
    glDrawElementsInstanced(4, 6, 0x1403, 0, 3); glEndQuery(0x8C87); glFinish();
    char s[3][32]; pxs(16, 32, s[0]); pxs(32, 32, s[1]); pxs(48, 32, s[2]);
    CHECK(px_is(16, 32, 128, 0, 255, 255) && px_is(32, 32, 128, 0, 255, 255) && px_is(48, 32, 128, 0, 255, 255) && px_is(24, 32, 0, 0, 0, 255) && px_is(40, 32, 0, 0, 0, 255),
          "3 个实例落在 x = 16 / 32 / 48（%s | %s | %s），实例之间是清除色，aK = 0.5 → 红 128", s[0], s[1], s[2]);
    GLuint prims = 0; glGetQueryObjectuiv(q, 0x8866, &prims); CHECK(prims == 6, "生成图元数：6 索引 × 3 实例 = 6（实测 %u）", prims);
    glVertexAttrib1f((GLuint)ak, 1.0f); glClear(0x4000); glDrawElementsInstanced(4, 6, 0x1403, 0, 0); glFinish();
    CHECK(px_is(32, 32, 0, 0, 0, 255), "实例数 0：不画");
    glDrawElementsInstanced(4, 6, 0x1403, 0, -1); GLenum e1 = glGetError(); glVertexAttribDivisor(16, 1); GLenum e2 = glGetError(); glVertexAttrib1f(16, 0); GLenum e3 = glGetError();
    CHECK(e1 == INVALID_VALUE && e2 == INVALID_VALUE && e3 == INVALID_VALUE, "负实例数 / 属性号 16 → INVALID_VALUE（%#x %#x %#x）", e1, e2, e3);
    const float off4[] = {-.5f, 0, -.5f, 0, -.5f, 0, -.5f, 0}; glBindBuffer(0x8892, b[1]); glBufferData(0x8892, sizeof off4, off4, 0x88E4);
    glVertexAttribDivisor((GLuint)ao, 0); glDrawElementsInstanced(4, 6, 0x1403, 0, 1); glFinish();
    CHECK(px_is(16, 32, 255, 0, 255, 255) && px_is(32, 32, 0, 0, 0, 255), "除数改回 0：aOff 逐顶点（4 个顶点都偏 −0.5 → 只在 x = 16），aK 新值 1.0 → 红 255");
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

// ---------- K 10:10:10:2 打包顶点属性 ----------
static uint32_t pk1010102(int r, int g, int b, int a) { return (uint32_t)(r & 0x3ff) | (uint32_t)(g & 0x3ff) << 10 | (uint32_t)(b & 0x3ff) << 20 | (uint32_t)(a & 3) << 30; }
static void t_packed_attr(void) {
    printf("K 10:10:10:2 打包顶点属性\n");
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    glUseProgram(g_tri); glUniform2f(glGetUniformLocation(g_tri, "uOff"), 0, 0); glUniform4f(glGetUniformLocation(g_tri, "uTint"), 1, 1, 1, 1);
    GLuint vao, b[2]; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(2, b);
    const float t = -1.0f / 3, u = 1.0f / 3;
    const float pos[] = {-1, -1, t, -1, -1, 1, t, -1, t, 1, -1, 1,  t, -1, u, -1, t, 1, u, -1, u, 1, t, 1,  u, -1, 1, -1, u, 1, 1, -1, 1, 1, u, 1};
    uint32_t col[18]; for (int i = 0; i < 6; i++) { col[i] = pk1010102(1023, 511, 0, 3); col[6 + i] = pk1010102(256, 511, -512, 1); col[12 + i] = pk1010102(1, 0, 0, 0); }
    GLint ap = glGetAttribLocation(g_tri, "aPos"), ac = glGetAttribLocation(g_tri, "aCol");
    glBindBuffer(0x8892, b[0]); glBufferData(0x8892, sizeof pos, pos, 0x88E4); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 0, 0); glEnableVertexAttribArray((GLuint)ap);
    glBindBuffer(0x8892, b[1]); glBufferData(0x8892, sizeof col, col, 0x88E4); glEnableVertexAttribArray((GLuint)ac);
    glVertexAttribPointer((GLuint)ac, 4, 0x8368, 1, 0, 0); glDrawArrays(4, 0, 6);    // GL_UNSIGNED_INT_2_10_10_10_REV，归一化
    glVertexAttribPointer((GLuint)ac, 4, 0x8D9F, 1, 0, 0); glDrawArrays(4, 6, 6);    // GL_INT_2_10_10_10_REV，归一化
    GLenum e = glGetError();
    glVertexAttribPointer((GLuint)ac, 4, 0x8368, 0, 0, 0); glDrawArrays(4, 12, 6);   // 不归一化
    glFinish(); char s[3][32]; pxs(10, 32, s[0]); pxs(32, 32, s[1]); pxs(53, 32, s[2]);
    CHECK(px_is(10, 32, 255, 127, 0, 255) && e == 0, "无符号归一化 (1023, 511, 0) → 255 127 0 255（实测 %s，错误 %#x）", s[0], e);
    CHECK(px_is(32, 32, 128, 255, 0, 255), "有符号归一化 (256, 511, −512) → 128 255 0 255（实测 %s）", s[1]);
    CHECK(px_is(53, 32, 0, 0, 255, 255), "不归一化：Metal 无对应格式，不画、保持清除色 0 0 255 255（实测 %s）", s[2]);
    glDeleteBuffers(2, b); glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

// ---------- I 多边形模式 / 线宽 / 读缓冲 ----------
static void t_raster_read(void) {
    printf("I 多边形模式 / 线宽 / 读缓冲\n");
    G(glPolygonMode, void, GLenum, GLenum) G(glLineWidth, void, GLfloat) G(glReadBuffer, void, GLenum)
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float tri[] = {-.8f, -.8f, 1, 0, 0,  .8f, -.8f, 1, 0, 0,  -.8f, .8f, 1, 0, 0}; tri_use(tri, 3);
    glPolygonMode(0x0408, 0x1B01); glDrawArrays(4, 0, 3); glFinish(); int edge = 0; for (int y = 4; y <= 8; y++) edge |= px_is(20, y, 255, 0, 0, 255);
    CHECK(px_is(20, 20, 0, 0, 255, 255) && edge && !glGetError(), "GL_LINE：三角形内部 (20,20) 保持蓝、底边附近 (20, 4–8) 有红线");
    glPolygonMode(0x0408, 0x1B02); glDrawArrays(4, 0, 3); glFinish(); CHECK(px_is(20, 20, 255, 0, 0, 255), "GL_FILL：内部填满红");
    glPolygonMode(0x0404, 0x1B01); GLenum e1 = glGetError(); glPolygonMode(0x0408, 0x1234); GLenum e2 = glGetError();
    CHECK(e1 == INVALID_ENUM && e2 == INVALID_ENUM, "单面 / 未知模式 → INVALID_ENUM");
    glLineWidth(2); GLfloat lw = 0; glGetFloatv(0x0B21, &lw); GLenum e3 = glGetError(); glLineWidth(0); GLenum e4 = glGetError(); glLineWidth(1);
    CHECK(lw == 2 && e3 == 0 && e4 == INVALID_VALUE, "glLineWidth(2) 记作查询值 %g（Metal 按 1 画、首例报红）、0 → INVALID_VALUE", lw);
    // 读缓冲：COLOR0 清红、COLOR1 清绿
    GLuint f2, t[2]; glGenFramebuffers(1, &f2); glBindFramebuffer(0x8D40, f2); glGenTextures(2, t);
    for (int i = 0; i < 2; i++) { glBindTexture(0x0DE1, t[i]); glTexImage2D(0x0DE1, 0, 0x8058, 8, 8, 0, 0x1908, 0x1401, NULL); glFramebufferTexture2D(0x8D40, 0x8CE0 + i, 0x0DE1, t[i], 0); }
    glViewport(0, 0, 8, 8); GLenum db0[] = {0x8CE0}, db1[] = {0, 0x8CE1};
    glDrawBuffers(1, db0); glClearColor(1, 0, 0, 1); glClear(0x4000); glDrawBuffers(2, db1); glClearColor(0, 1, 0, 1); glClear(0x4000);
    glReadBuffer(0x8CE1); int g = px_is(3, 3, 0, 255, 0, 255); glReadBuffer(0x8CE0); int r = px_is(3, 3, 255, 0, 0, 255);
    CHECK(g && r, "FBO 读缓冲：COLOR1 读到绿、COLOR0 读到红");
    glReadBuffer(0); uint8_t p[4]; glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, p); GLenum e5 = glGetError(); glReadBuffer(0x0405); GLenum e6 = glGetError(); glReadBuffer(0x8CE0 + 8); GLenum e7 = glGetError();
    CHECK(e5 == INVALID_OPERATION && e6 == INVALID_OPERATION && e7 == INVALID_OPERATION, "读缓冲 NONE 时 ReadPixels / FBO 上读 BACK / COLOR_ATTACHMENT8 → INVALID_OPERATION（%#x %#x %#x）", e5, e6, e7);
    glBindFramebuffer(0x8D40, 0); glReadBuffer(0x8CE0); GLenum e8 = glGetError(); glReadBuffer(0x0402); GLenum e9 = glGetError();
    CHECK(e8 == INVALID_OPERATION && e9 == 0, "0 号帧缓冲读 COLOR_ATTACHMENT0 → INVALID_OPERATION、BACK_LEFT 收");
    // 前缓冲：清蓝、交换；再清黄（不交换）。FRONT 读上一帧蓝、BACK 读黄
    glViewport(0, 0, 1920, 1080); glClearColor(0, 0, 1, 1); glClear(0x4000); eglSwapBuffers(g_dpy, g_sf);
    glClearColor(1, 1, 0, 1); glClear(0x4000); glFinish();
    glReadBuffer(0x0404); char fs[32], bs[32]; pxs(100, 100, fs); int front = px_is(100, 100, 0, 0, 255, 255); glReadBuffer(0x0405); pxs(100, 100, bs); int back = px_is(100, 100, 255, 255, 0, 255);
    CHECK(front && back && !glGetError(), "0 号帧缓冲：GL_FRONT 读上一帧交换出去的蓝（%s）、GL_BACK 读当前黄（%s）", fs, bs);
    glDeleteFramebuffers(1, &fb); glDeleteFramebuffers(1, &f2); glDeleteTextures(2, t);
}

// ---------- J uniform / 名字查询 / 程序杂项 ----------
static void t_uniforms(void) {
    printf("J 整数向量 / 矩阵 uniform\n");
    G(glUniform2iv, void, GLint, GLsizei, const GLint *) G(glUniform3iv, void, GLint, GLsizei, const GLint *) G(glUniform4iv, void, GLint, GLsizei, const GLint *)
    G(glUniformMatrix2fv, void, GLint, GLsizei, GLuint, const GLfloat *) G(glUniformMatrix3fv, void, GLint, GLsizei, GLuint, const GLfloat *)
    GLuint fb = fbo_rb(8, 8, NULL), prog = program("api_uni"); glUseProgram(prog);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}; glBufferData(0x8892, sizeof quad, quad, 0x88E4);
    GLint ap = glGetAttribLocation(prog, "aPos"); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 8, 0); glEnableVertexAttribArray((GLuint)ap);
    GLint sel = glGetUniformLocation(prog, "uSel");
    const GLint i2[] = {10, 200}, i3[] = {1, 2, 3}, i4[] = {4, 5, 6, 7}; const GLfloat m2[] = {.2f, .4f, .6f, .8f}, m3r[] = {.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f};
    glUniform2iv(glGetUniformLocation(prog, "uI2"), 1, i2); glUniform3iv(glGetUniformLocation(prog, "uI3"), 1, i3); glUniform4iv(glGetUniformLocation(prog, "uI4"), 1, i4);
    glUniformMatrix2fv(glGetUniformLocation(prog, "uM2"), 1, 0, m2); glUniformMatrix3fv(glGetUniformLocation(prog, "uM3"), 1, 1, m3r);
    CHECK(!glGetError(), "写 ivec2 / ivec3 / ivec4 / mat2 / mat3（transpose）无错");
    static const int want[7][4] = {{10, 200, 0, 255}, {1, 2, 3, 255}, {4, 5, 6, 7}, {51, 102, 153, 204}, {26, 102, 179, 255}, {51, 128, 204, 255}, {77, 153, 230, 255}};
    static const char *what[7] = {"ivec2 (10, 200)", "ivec3 (1, 2, 3)", "ivec4 (4, 5, 6, 7)", "mat2 两列 (.2 .4)(.6 .8)", "mat3 第 0 列 = 行主序第 0 列 (.1 .4 .7)", "mat3 第 1 列 (.2 .5 .8)", "mat3 第 2 列 (.3 .6 .9)"};
    for (int k = 0; k < 7; k++) { char s[32]; glUniform1i(sel, k); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
        CHECK(px_is(4, 4, want[k][0], want[k][1], want[k][2], want[k][3]), "%s → 期望 %d %d %d %d，实测 %s", what[k], want[k][0], want[k][1], want[k][2], want[k][3], s); }
    const GLfloat m3c[] = {0, 0, 0, 0, 0, 0, .25f, .5f, .75f}; char s[32];
    glUniformMatrix3fv(glGetUniformLocation(prog, "uM3"), 1, 0, m3c); glUniform1i(sel, 6); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
    CHECK(px_is(4, 4, 64, 128, 191, 255), "mat3 不转置：第 2 列 (.25 .5 .75) → 64 128 191（%s）", s);
    glUniform3iv(glGetUniformLocation(prog, "uI2"), 1, i3); GLenum e1 = glGetError(); glUniformMatrix2fv(glGetUniformLocation(prog, "uM3"), 1, 0, m2); GLenum e2 = glGetError();
    glUniform4iv(-1, 1, i4); GLenum e3 = glGetError(); glUniform2iv(glGetUniformLocation(prog, "uI2"), -1, i2); GLenum e4 = glGetError();
    CHECK(e1 == INVALID_OPERATION && e2 == INVALID_OPERATION && e3 == 0 && e4 == INVALID_VALUE, "类型不符 INVALID_OPERATION、位置 −1 静默、count < 0 INVALID_VALUE（%#x %#x %#x %#x）", e1, e2, e3, e4);
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}
static void t_names_programs(void) {
    printf("J 名字查询 / 程序杂项\n");
    G(glIsBuffer, GLboolean, GLuint) G(glIsFramebuffer, GLboolean, GLuint) G(glIsRenderbuffer, GLboolean, GLuint) G(glIsProgram, GLboolean, GLuint) G(glIsShader, GLboolean, GLuint)
    G(glDetachShader, void, GLuint, GLuint) G(glValidateProgram, void, GLuint) G(glReleaseShaderCompiler, void, void)
    G(glShaderBinary, void, GLsizei, const GLuint *, GLenum, const void *, GLsizei)
    GLuint b, f, r; glGenBuffers(1, &b); glGenFramebuffers(1, &f); glGenRenderbuffers(1, &r);
    int before = glIsBuffer(b) || glIsFramebuffer(f) || glIsRenderbuffer(r);
    glBindBuffer(0x8892, b); glBindFramebuffer(0x8D40, f); glBindRenderbuffer(0x8D41, r); glBindFramebuffer(0x8D40, 0);
    int bound = glIsBuffer(b) && glIsFramebuffer(f) && glIsRenderbuffer(r);
    glDeleteBuffers(1, &b); glDeleteFramebuffers(1, &f); glDeleteRenderbuffers(1, &r);
    int after = glIsBuffer(b) || glIsFramebuffer(f) || glIsRenderbuffer(r) || glIsBuffer(0) || glIsFramebuffer(0) || glIsRenderbuffer(0);
    CHECK(!before && bound && !after, "glIsBuffer / Framebuffer / Renderbuffer：Gen 后未绑 = 假、绑过 = 真、删后与 0 = 假（%d %d %d）", before, bound, after);
    GLuint vs = shader(0x8B31, "tri.vert"), fs = shader(0x8B30, "tri.frag"), p = glCreateProgram();
    int is = glIsShader(vs) && glIsProgram(p) && !glIsShader(p + 1000) && !glIsProgram(0);
    glAttachShader(p, vs); glAttachShader(p, fs); glBindAttribLocation(p, 5, "aPos"); glLinkProgram(p); GLint ok = 0; glGetProgramiv(p, 0x8B82, &ok);
    GLint loc = glGetAttribLocation(p, "aPos");
    glValidateProgram(p); GLint val = -1; glGetProgramiv(p, 0x8B83, &val);
    CHECK(is && ok && loc == 5 && val == 1 && !glGetError(), "glIsShader / glIsProgram 真；BindAttribLocation(aPos→5) 链接后 = %d；已链接程序校验 = %d", loc, val);
    glDetachShader(p, fs); GLenum e0 = glGetError(); glDetachShader(p, fs); GLenum e1 = glGetError(); glLinkProgram(p); glGetProgramiv(p, 0x8B82, &ok);
    glValidateProgram(p); glGetProgramiv(p, 0x8B83, &val);
    CHECK(e0 == 0 && e1 == INVALID_OPERATION && ok == 0 && val == 0, "分离片元后再分离 → INVALID_OPERATION；重新链接失败、校验 0（%#x %#x link %d val %d）", e0, e1, ok, val);
    glDeleteShader(vs); glDeleteProgram(p); CHECK(!glIsShader(vs) && !glIsProgram(p), "删后 glIsShader / glIsProgram 假");
    GLuint pv = program("api_vol"); glUseProgram(pv); glUniform1i(glGetUniformLocation(pv, "uArr"), 0); glUniform1i(glGetUniformLocation(pv, "uVol"), 0);
    glValidateProgram(pv); glGetProgramiv(pv, 0x8B83, &val); GLint v1 = val; glUniform1i(glGetUniformLocation(pv, "uVol"), 1); glValidateProgram(pv); glGetProgramiv(pv, 0x8B83, &val);
    CHECK(v1 == 0 && val == 1, "2D 数组与 3D 采样器指同一单元校验 0、分开后 1（%d %d）", v1, val);
    glReleaseShaderCompiler(); GLenum e2 = glGetError(); GLuint again = shader(0x8B31, "tri.vert");   // shader() 编不过会直接退出
    CHECK(e2 == 0 && glIsShader(again), "ReleaseShaderCompiler 无错，之后照样编译");
    const uint32_t blob[4] = {0}; glShaderBinary(1, &again, 0x9260, blob, sizeof blob); GLenum e3 = glGetError(); glShaderBinary(-1, &again, 0x9260, blob, 4); GLenum e4 = glGetError();
    CHECK(e3 == INVALID_ENUM && e4 == INVALID_VALUE, "ShaderBinary：不支持的格式 → INVALID_ENUM（不假装成功）、count < 0 → INVALID_VALUE（%#x %#x）", e3, e4);
}

// ---------- L 逐帧换贴图存储不漏 ----------
static size_t heap_in_use(void) { malloc_statistics_t st; malloc_zone_statistics(NULL, &st); return st.size_in_use; }
// 进程堆也等落定再读（11-09 根修）：glFinish 只等到 GPU 做完，命令缓冲的完成回调在 Metal 自己的线程上随后才跑、才放掉最后几帧的命令缓冲 / 贴图 / 拷贝缓冲；
//   负载高时回调晚到，立刻读会多出几 MB（守望 11-09 一次、本机 16 路并发 8/16 读到 4 MB 判红）。漏是与帧数成正比的 ≥ 197 MB，等不掉
static size_t settled_heap_bytes(void) {
    size_t last = heap_in_use(); int same = 0;
    for (int i = 0; i < 300 && same < 5; i++) { usleep(10000); size_t now = heap_in_use(); same = (now > last ? now - last : last - now) < 65536 ? same + 1 : 0; last = now; }
    return last;
}
// glFinish 之后命令缓冲的完成回调与资源释放还可能晚一点落定：等已分配字节连续 5 次（50 ms）不变
// ---------- M 多线程共享上下文 ----------
static EGLConfig g_cfg;
static struct {
    EGLContext share, ctx; dispatch_semaphore_t to_b, to_main; GLuint fb, vbo, tex;
    EGLContext (*create)(EGLDisplay, EGLConfig, EGLContext, const EGLint *); EGLBoolean (*make)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLContext (*current)(void); EGLBoolean (*destroy)(EGLDisplay, EGLContext); EGLBoolean (*release)(void);
    int bound_self; GLenum status0, clear0_err; int destroyed_still_current, released_none; GLenum b_err;
} mt;
static void mt_attribs(GLuint vbo) {   // 当前上下文的 0 号顶点数组接 tri 程序的 aPos / aCol
    glUseProgram(g_tri); glBindVertexArray(0); glBindBuffer(0x8892, vbo);
    GLint p = glGetAttribLocation(g_tri, "aPos"), c = glGetAttribLocation(g_tri, "aCol");
    glVertexAttribPointer((GLuint)p, 2, 0x1406, 0, 20, (void *)0); glEnableVertexAttribArray((GLuint)p);
    glVertexAttribPointer((GLuint)c, 3, 0x1406, 0, 20, (void *)8); glEnableVertexAttribArray((GLuint)c);
}
static void *mt_thread_b(void *arg) { @autoreleasepool {
    (void)arg; dispatch_semaphore_wait(mt.to_b, DISPATCH_TIME_FOREVER);
    const EGLint ctx_attr[] = {0x3098, 3, 0x30FB, 2, 0x3038};
    mt.ctx = mt.create(g_dpy, g_cfg, mt.share, ctx_attr); mt.make(g_dpy, NULL, NULL, mt.ctx);   // 无表面上下文
    mt.bound_self = mt.ctx && mt.current() == mt.ctx;
    mt.status0 = glCheckFramebufferStatus(0x8D40); glClear(0x4000); mt.clear0_err = glGetError();
    const float right[] = {QUAD(0, -1, .5f, 1, 0, 1, 0), QUAD(.5f, -1, 1, 1, 0, 1, 0)};   // 右第一格、右第二格：绿
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo); glBufferData(0x8892, sizeof right, right, 0x88E4); mt.vbo = vbo;
    glBindFramebuffer(0x8D40, mt.fb); glViewport(0, 0, 64, 64); mt_attribs(vbo); glDrawArrays(4, 0, 6);
    uint8_t px[4] = {1, 2, 3, 4}; glGenTextures(1, &mt.tex); glBindTexture(0x0DE1, mt.tex); glTexImage2D(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, px);
    dispatch_semaphore_signal(mt.to_main); dispatch_semaphore_wait(mt.to_b, DISPATCH_TIME_FOREVER);
    glDrawArrays(4, 6, 6);   // 不改任何状态：主线程刚往同一编码器录过自己的顶点缓冲
    mt.b_err = glGetError();
    mt.destroy(g_dpy, mt.ctx); mt.destroyed_still_current = mt.current() == mt.ctx; glFlush();   // 删后仍是当前、还能调 GL
    mt.release(); mt.released_none = mt.current() == NULL;
    dispatch_semaphore_signal(mt.to_main); return NULL;
}}
static void t_threads(void) {
    printf("M 多线程共享上下文（当前上下文按线程）\n");
    G(eglCreateContext, EGLContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *) G(eglMakeCurrent, EGLBoolean, EGLDisplay, EGLSurface, EGLSurface, EGLContext)
    G(eglGetCurrentContext, EGLContext, void) G(eglDestroyContext, EGLBoolean, EGLDisplay, EGLContext) G(eglReleaseThread, EGLBoolean, void)
    mt.create = eglCreateContext; mt.make = eglMakeCurrent; mt.current = eglGetCurrentContext; mt.destroy = eglDestroyContext; mt.release = eglReleaseThread;
    mt.share = eglGetCurrentContext(); mt.to_b = dispatch_semaphore_create(0); mt.to_main = dispatch_semaphore_create(0);
    mt.fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float left[] = {QUAD(-1, -1, -.5f, 1, 1, 0, 0), QUAD(-.5f, -1, 0, 1, 1, 0, 0)};   // 左第一格、左第二格：红
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo); glBufferData(0x8892, sizeof left, left, 0x88E4);
    mt_attribs(vbo); glUniform2f(glGetUniformLocation(g_tri, "uOff"), 0, 0); glUniform4f(glGetUniformLocation(g_tri, "uTint"), 1, 1, 1, 1);
    glDrawArrays(4, 0, 6);
    pthread_t th; pthread_create(&th, NULL, mt_thread_b, NULL);
    dispatch_semaphore_signal(mt.to_b); dispatch_semaphore_wait(mt.to_main, DISPATCH_TIME_FOREVER);
    EGLContext cur = eglGetCurrentContext(); GLint fbb = -1; glGetIntegerv(0x8CA6, &fbb); GLenum e = glGetError();
    CHECK(mt.bound_self, "线程 B 绑上自己的共享上下文");
    CHECK(cur == mt.share, "B 绑定后主线程的当前上下文仍是 A（实测 %s）", cur == mt.share ? "A" : cur == mt.ctx ? "B" : "别的");
    CHECK(fbb == (GLint)mt.fb && e == 0, "主线程帧缓冲绑定仍是 %u（实测 %d）、错误码不串（实测 %#x）", mt.fb, fbb, e);
    CHECK(mt.status0 == 0x8219, "无表面上下文 0 号帧缓冲状态 GL_FRAMEBUFFER_UNDEFINED 0x8219（实测 %#x）", mt.status0);
    CHECK(mt.clear0_err == 0x0506, "无表面上下文清 0 号帧缓冲 → INVALID_FRAMEBUFFER_OPERATION 0x506（实测 %#x）", mt.clear0_err);
    glDrawArrays(4, 6, 6);   // 主线程自己的 0 号顶点数组：左第二格
    dispatch_semaphore_signal(mt.to_b); dispatch_semaphore_wait(mt.to_main, DISPATCH_TIME_FOREVER); pthread_join(th, NULL);
    glFinish(); char s[4][32]; pxs(8, 32, s[0]); pxs(24, 32, s[1]); pxs(40, 32, s[2]); pxs(56, 32, s[3]);
    CHECK(px_is(8, 32, 255, 0, 0, 255) && px_is(24, 32, 255, 0, 0, 255), "主线程两笔用自己的 0 号顶点数组：左两格红（实测 %s / %s）", s[0], s[1]);
    CHECK(px_is(40, 32, 0, 255, 0, 255) && px_is(56, 32, 0, 255, 0, 255) && !mt.b_err, "B 两笔（第二笔紧跟主线程录同一编码器）：右两格绿（实测 %s / %s，B 错误 %#x）", s[2], s[3], mt.b_err);
    CHECK(mt.destroyed_still_current && mt.released_none, "删自己的当前上下文后仍是当前（%d）、eglReleaseThread 后当前为空（%d）", mt.destroyed_still_current, mt.released_none);
    GLuint f2; glGenFramebuffers(1, &f2); glBindFramebuffer(0x8D40, f2); glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, mt.tex, 0); char t[32]; pxs(0, 0, t);
    CHECK(px_is(0, 0, 1, 2, 3, 4) && !glGetError(), "B 建的纹理主线程照样读到 1 2 3 4（共享组，实测 %s）", t);
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &f2); glDeleteFramebuffers(1, &mt.fb); glDeleteTextures(1, &mt.tex);
    GLuint bufs[2] = {vbo, mt.vbo}; glDeleteBuffers(2, bufs); glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
}

static size_t settled_device_bytes(void) {
    size_t last = gl_dev.currentAllocatedSize; int same = 0;
    for (int i = 0; i < 300 && same < 5; i++) { usleep(10000); size_t now = gl_dev.currentAllocatedSize; same = now == last ? same + 1 : 0; last = now; }
    return last;
}
static void t_frame_storage(void) {
    printf("L 逐帧换贴图存储不漏\n");
    G(eglSwapInterval, EGLBoolean, EGLDisplay, EGLint) G(glTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)
    enum { W = 384, H = 224, WARM = 60, N = 600 };
    GLuint prog = program("texture"); glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uScale"), 1, 1); glUniform2f(glGetUniformLocation(prog, "uOff"), 0, 0);
    glUniform2f(glGetUniformLocation(prog, "uUVScale"), 1, 1); glUniform2f(glGetUniformLocation(prog, "uUVOff"), 0, 0); glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}; glBufferData(0x8892, sizeof quad, quad, 0x88E4);
    GLint ap = glGetAttribLocation(prog, "aPos"); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 8, 0); glEnableVertexAttribArray((GLuint)ap);
    uint8_t *pix = malloc(W * H * 4); for (int i = 0; i < W * H * 4; i++) pix[i] = (uint8_t)i;
    glBindFramebuffer(0x8D40, 0); glViewport(0, 0, 1920, 1080); eglSwapInterval(g_dpy, 0);   // 不按 60 帧节拍睡，600 帧几秒跑完
    for (int mode = 0; mode < 2; mode++) {
        GLuint tex; glGenTextures(1, &tex); glActiveTexture(0x84C0); glBindTexture(0x0DE1, tex);
        glTexParameteri(0x0DE1, 0x2801, 0x2600); glTexParameteri(0x0DE1, 0x2800, 0x2600);   // NEAREST、不要 mip：单级即完整
        glTexImage2D(0x0DE1, 0, 0x1908, W, H, 0, 0x1908, 0x1401, pix);
        size_t d0 = 0, h0 = 0;
        for (int f = 0; f < WARM + N; f++) {
            if (f == WARM) { glFinish(); d0 = settled_device_bytes(); h0 = settled_heap_bytes(); }
            pix[0] = (uint8_t)f;
            if (mode == 0) glTexImage2D(0x0DE1, 0, 0x1908, W, H, 0, 0x1908, 0x1401, pix);   // 每帧重新指定：必换新存储
            else glTexSubImage2D(0x0DE1, 0, 0, 0, W, H, 0x1908, 0x1401, pix);              // 上一帧的绘制还在途：换新存储
            glDrawArrays(4, 0, 6); eglSwapBuffers(g_dpy, g_sf);
        }
        glFinish(); size_t d1 = settled_device_bytes(), h1 = settled_heap_bytes();
        double dmb = ((double)d1 - (double)d0) / 1048576, hmb = ((double)h1 - (double)h0) / 1048576;
        CHECK(dmb < 8 && hmb < 32 && !glGetError(), "%s 每帧换存储 %d 帧：设备已分配增量 %.1f MB（< 8）、进程堆增量 %.2f MB（< 32）；漏则 ≥ %.0f MB",
              mode ? "glTexSubImage2D（改在途贴图）" : "glTexImage2D", N, dmb, hmb, N * (double)(W * H * 4) / 1048576);
        glDeleteTextures(1, &tex);
    }
    eglSwapInterval(g_dpy, 1); free(pix); glDeleteBuffers(1, &vbo); glDeleteProgram(prog);
}

// ---------- N 上下文 API 与范围映射 ----------
static void t_api_version(void) {
    printf("N 上下文 API 与范围映射：版本串\n");
    G(glGetString, const GLchar *, GLenum) G(eglBindAPI, EGLBoolean, EGLenum) G(eglCreateContext, EGLContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *)
    G(eglMakeCurrent, EGLBoolean, EGLDisplay, EGLSurface, EGLSurface, EGLContext) G(eglGetCurrentContext, EGLContext, void) G(eglDestroyContext, EGLBoolean, EGLDisplay, EGLContext)
    const char *ver = glGetString(0x1F02), *sl = glGetString(0x8B8C); int ma = -1, mi = -1, n = ver ? sscanf(ver, "OpenGL ES %d.%d", &ma, &mi) : -1; GLint gma = 0, gmi = 0;
    glGetIntegerv(0x821B, &gma); glGetIntegerv(0x821C, &gmi);
    CHECK(n == 2 && ma == 3 && mi == 2 && sl && !strncmp(sl, "OpenGL ES GLSL ES ", 18) && gma == 3 && gmi == 2 && !glGetError(),
          "ES 上下文：VERSION「%s」读出 %d 项 %d.%d、GLSL「%s」、MAJOR / MINOR %d / %d", ver ? ver : "(空)", n, ma, mi, sl ? sl : "(空)", gma, gmi);
    GLint bits[2] = {0, 0}, align = 0; glGetIntegerv(0x0D54, &bits[0]); glGetIntegerv(0x0D55, &bits[1]); glGetIntegerv(0x8A34, &align);
    CHECK(bits[0] == 8 && bits[1] == 8 && align == 256 && !glGetError(), "BLUE / ALPHA_BITS %d / %d、UNIFORM_BUFFER_OFFSET_ALIGNMENT %d", bits[0], bits[1], align);
    EGLContext es = eglGetCurrentContext(); eglBindAPI(0x30A2); EGLContext gl = eglCreateContext(g_dpy, g_cfg, NULL, NULL); eglBindAPI(0x30A0);
    if (gl && eglMakeCurrent(g_dpy, g_sf, g_sf, gl)) {
        const char *dv = glGetString(0x1F02); int dn = dv ? sscanf(dv, "OpenGL ES %d.%d", &ma, &mi) : -1; glGetIntegerv(0x821B, &gma); glGetIntegerv(0x821C, &gmi);
        CHECK(dv && !strncmp(dv, "4.5", 3) && dn == 0 && gma == 4 && gmi == 5, "桌面上下文：VERSION「%s」、ES 格式读出 %d 项、MAJOR / MINOR %d / %d", dv ? dv : "(空)", dn, gma, gmi);
    } else CHECK(0, "桌面上下文建立");
    eglMakeCurrent(g_dpy, g_sf, g_sf, es); if (gl) eglDestroyContext(g_dpy, gl);
}
// #1440：一款 GL 游戏开局问当前显示 / 表面 / 上下文属性；期望按 EGL 1.5 §3.7.3–3.7.4 与本前端的单显示（1）、单配置（1）、读写同表面手写
static void t_egl_current(void) {
    printf("N2 当前显示 / 表面与上下文属性\n");
    G(eglGetCurrentDisplay, EGLDisplay, void) G(eglGetCurrentSurface, EGLSurface, EGLint) G(eglQueryContext, EGLBoolean, EGLDisplay, EGLContext, EGLint, EGLint *)
    G(eglGetError, EGLint, void) G(eglBindAPI, EGLBoolean, EGLenum) G(eglCreateContext, EGLContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *)
    G(eglMakeCurrent, EGLBoolean, EGLDisplay, EGLSurface, EGLSurface, EGLContext) G(eglGetCurrentContext, EGLContext, void) G(eglDestroyContext, EGLBoolean, EGLDisplay, EGLContext)
    EGLContext es = eglGetCurrentContext();
    CHECK(eglGetCurrentDisplay() == g_dpy, "当前显示 = eglGetDisplay 给的那个");
    CHECK(eglGetCurrentSurface(0x3059) == g_sf && eglGetCurrentSurface(0x305A) == g_sf, "EGL_DRAW / EGL_READ 都是当前窗口表面");
    CHECK(eglGetCurrentSurface(0x3000) == NULL && eglGetError() == 0x300C, "别的 readdraw：EGL_NO_SURFACE + EGL_BAD_PARAMETER");
    EGLint t = 0, ver = 0, cfg = 0, rb = 0;
    CHECK(eglQueryContext(g_dpy, es, 0x3097, &t) && t == 0x30A0 && eglQueryContext(g_dpy, es, 0x3098, &ver) && ver == 3,
          "ES 上下文：CLIENT_TYPE %#x（期望 0x30a0）、CLIENT_VERSION %d（期望 3）", t, ver);
    CHECK(eglQueryContext(g_dpy, es, 0x3028, &cfg) && cfg == 1 && eglQueryContext(g_dpy, es, 0x3086, &rb) && rb == 0x3084,
          "CONFIG_ID %d（期望 1）、绑着窗口表面 RENDER_BUFFER %#x（期望 EGL_BACK_BUFFER 0x3084）", cfg, rb);
    CHECK(!eglQueryContext(g_dpy, es, 0x3030, &t) && eglGetError() == 0x3004, "不认的属性：失败 + EGL_BAD_ATTRIBUTE");
    CHECK(!eglQueryContext((EGLDisplay)2, es, 0x3097, &t) && eglGetError() == 0x3008, "错的显示：失败 + EGL_BAD_DISPLAY");
    eglBindAPI(0x30A2); EGLContext gl = eglCreateContext(g_dpy, g_cfg, NULL, NULL); eglBindAPI(0x30A0);
    CHECK(gl && eglQueryContext(g_dpy, gl, 0x3097, &t) && t == 0x30A2 && eglQueryContext(g_dpy, gl, 0x3098, &ver) && ver == 4 &&
          eglQueryContext(g_dpy, gl, 0x3086, &rb) && rb == 0x3038, "桌面上下文（没绑）：CLIENT_TYPE %#x、VERSION %d、RENDER_BUFFER %#x（期望 0x30a2 / 4 / EGL_NONE）", t, ver, rb);
    if (gl) eglDestroyContext(g_dpy, gl);
    eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    CHECK(eglGetCurrentDisplay() == NULL && eglGetCurrentSurface(0x3059) == NULL, "没有当前上下文：EGL_NO_DISPLAY / EGL_NO_SURFACE");
    eglMakeCurrent(g_dpy, g_sf, g_sf, es);
}
static void t_map_range(void) {
    printf("N 上下文 API 与范围映射：范围映射\n");
    G(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield) G(glFlushMappedBufferRange, void, GLenum, GLintptr, GLsizeiptr) G(glUnmapBuffer, GLboolean, GLenum)
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float left[] = {QUAD(-1, -1, 0, 1, 1, 0, 0)}; tri_use(left, 6);
    float *ro = glMapBufferRange(0x8892, 8, 8, 1); int same = ro && ro[0] == left[2] && ro[1] == left[3];
    float *again = glMapBufferRange(0x8892, 0, 4, 1); GLenum e_mapped = glGetError(); GLboolean un = glUnmapBuffer(0x8892);
    CHECK(same && !again && e_mapped == INVALID_OPERATION && un == 1, "只读范围映射（偏移 8 长 8）读到 left[2] / left[3]；已映射再映射 → INVALID_OPERATION");
    glDrawArrays(4, 0, 6);   // 这笔读旧内容（左半红）
    const float right[] = {QUAD(0, -1, 1, 1, 0, 1, 0)};
    float *w = glMapBufferRange(0x8892, 0, sizeof right, 2 | 0x10); if (w) memcpy(w, right, sizeof right);
    glFlushMappedBufferRange(0x8892, 0, sizeof right); GLenum e_flush = glGetError();
    glFlushMappedBufferRange(0x8892, 4, sizeof right); GLenum e_flush_over = glGetError();
    CHECK(w && e_flush == 0 && e_flush_over == INVALID_VALUE && glUnmapBuffer(0x8892) == 1, "可写 + 显式刷新映射：映射内刷新无错、越过映射长 → INVALID_VALUE、Unmap 回 TRUE");
    glDrawArrays(4, 0, 6); glFinish(); char a[32], b[32]; pxs(16, 32, a); pxs(48, 32, b);
    CHECK(px_is(16, 32, 255, 0, 0, 255) && px_is(48, 32, 0, 255, 0, 255), "映射前的那笔画旧内容（左红 %s）、范围映射写后的画新内容（右绿 %s）", a, b);
    GLenum e[6];
    glMapBufferRange(0x8892, 0, 0, 2); e[0] = glGetError();
    glMapBufferRange(0x8892, 8, sizeof right, 2); e[1] = glGetError();
    glMapBufferRange(0x8892, 0, 4, 1 | 4); e[2] = glGetError();
    glMapBufferRange(0x8892, 0, 4, 0x10); e[3] = glGetError();
    glMapBufferRange(0x8892, 0, 4, 1 | 0x10); e[4] = glGetError();
    glMapBufferRange(0x8892, 0, 4, 2); glFlushMappedBufferRange(0x8892, 0, 4); e[5] = glGetError(); glUnmapBuffer(0x8892);
    CHECK(e[0] == INVALID_VALUE && e[1] == INVALID_VALUE && e[2] == INVALID_OPERATION && e[3] == INVALID_OPERATION && e[4] == INVALID_OPERATION && e[5] == INVALID_OPERATION,
          "长 0 / 越界 → INVALID_VALUE；读 + 作废、读写位都没有、显式刷新不带写、映射没带显式刷新位却刷新 → INVALID_OPERATION（实测 %#x %#x %#x %#x %#x %#x）", e[0], e[1], e[2], e[3], e[4], e[5]);
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

static void t_pack_buffer(void) {
    printf("T 像素打包缓冲\n");
    G(glBufferStorage, void, GLenum, GLsizeiptr, const void *, GLbitfield) G(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield) G(glUnmapBuffer, GLboolean, GLenum)
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 1, 0, 1); glClear(0x4000); glFinish();
    uint8_t init[96]; memset(init, 0xAB, sizeof init);
    GLuint pb = 0; glGenBuffers(1, &pb); glBindBuffer(0x88EB, pb); GLenum e_bind = glGetError(); GLint got = -1; glGetIntegerv(0x88ED, &got);
    glBufferStorage(0x88EB, sizeof init, init, 1); GLenum e_st = glGetError();
    CHECK(e_bind == 0 && got == (GLint)pb && e_st == 0, "绑定 / 建存储无错（%#x / %#x）、PIXEL_PACK_BUFFER_BINDING = %d（期望 %u）", e_bind, e_st, got, pb);
    glReadPixels(0, 0, 4, 4, 0x1908, 0x1401, (void *)16); GLenum e_rd = glGetError();
    const uint8_t *m = glMapBufferRange(0x88EB, 0, sizeof init, 1); int green = m != NULL, keep = m != NULL;
    for (int i = 0; m && i < 16; i++) green &= m[16 + i * 4] == 0 && m[17 + i * 4] == 255 && m[18 + i * 4] == 0 && m[19 + i * 4] == 255;
    for (int i = 0; m && i < 16; i++) keep &= m[i] == 0xAB;
    CHECK(e_rd == 0 && green && keep, "读回进缓冲偏移 16：16 个像素都是 0 255 0 255、偏移 0..15 保持 0xAB（读回错 %#x、映射 %s）", e_rd, m ? "成" : "回空");
    glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, (void *)16); GLenum e_mapped = glGetError(); glUnmapBuffer(0x88EB);
    glReadPixels(0, 0, 4, 4, 0x1908, 0x1401, (void *)40); GLenum e_over = glGetError();
    CHECK(e_mapped == INVALID_OPERATION && e_over == INVALID_OPERATION, "映射着读回 → INVALID_OPERATION（%#x）；偏移 40 + 64 > 96 → INVALID_OPERATION（%#x）", e_mapped, e_over);
    glBindBuffer(0x88EB, 0); uint8_t px[4]; rpx(1, 1, px);
    CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255 && !glGetError(), "解绑后读回写客户端内存：%u %u %u %u（期望 0 255 0 255）", px[0], px[1], px[2], px[3]);
    glDeleteBuffers(1, &pb); glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
}

static void t_unpack_buffer(void) {
    printf("U 像素解包缓冲\n");
    G(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield) G(glUnmapBuffer, GLboolean, GLenum)
    G(glTexSubImage2D, void, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)
    uint8_t init[96]; memset(init, 0xAB, 16); for (int i = 0; i < 16; i++) { init[16 + i * 4] = 255; init[17 + i * 4] = 0; init[18 + i * 4] = 0; init[19 + i * 4] = 255; } memset(init + 80, 0, 16);
    GLuint ub = 0; glGenBuffers(1, &ub); glBindBuffer(0x88EC, ub); GLenum e_bind = glGetError(); GLint got = -1; glGetIntegerv(0x88EF, &got);
    glBufferData(0x88EC, sizeof init, init, 0x88E4); GLenum e_data = glGetError();
    CHECK(e_bind == 0 && e_data == 0 && got == (GLint)ub, "绑定 / 建存储无错（%#x / %#x）、PIXEL_UNPACK_BUFFER_BINDING = %d（期望 %u）", e_bind, e_data, got, ub);
    GLuint fb, tex; glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb); glGenTextures(1, &tex); glBindTexture(0x0DE1, tex);
    glTexImage2D(0x0DE1, 0, 0x8058, 4, 4, 0, 0x1908, 0x1401, (const void *)16); GLenum e_img = glGetError();
    glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0); char a[20], b[20]; pxs(1, 1, a); pxs(3, 3, b);
    CHECK(e_img == 0 && px_is(1, 1, 255, 0, 0, 255) && px_is(3, 3, 255, 0, 0, 255), "glTexImage2D 从偏移 16 取像素：(1,1) %s、(3,3) %s（期望都是 255 0 0 255，错 %#x）", a, b, e_img);
    uint8_t *m = glMapBufferRange(0x88EC, 80, 16, 2); if (m) for (int i = 0; i < 4; i++) { m[i * 4] = 0; m[i * 4 + 1] = 255; m[i * 4 + 2] = 0; m[i * 4 + 3] = 255; }
    glUnmapBuffer(0x88EC); GLenum e_map = glGetError();
    glTexSubImage2D(0x0DE1, 0, 0, 0, 2, 2, 0x1908, 0x1401, (const void *)80); GLenum e_sub = glGetError(); char c[20]; pxs(0, 0, a); pxs(1, 1, b); pxs(3, 3, c);
    CHECK(m && e_map == 0 && e_sub == 0 && px_is(0, 0, 0, 255, 0, 255) && px_is(1, 1, 0, 255, 0, 255) && px_is(3, 3, 255, 0, 0, 255),
          "映射写绿（%s）后 glTexSubImage2D 从偏移 80 取：(0,0) %s、(1,1) %s 期望绿，(3,3) %s 期望仍红（错 %#x / %#x）", m ? "映射成" : "映射回空", a, b, c, e_map, e_sub);
    glTexSubImage2D(0x0DE1, 0, 0, 0, 4, 4, 0x1908, 0x1401, (const void *)40); GLenum e_over = glGetError();
    glMapBufferRange(0x88EC, 0, 16, 1); glTexSubImage2D(0x0DE1, 0, 0, 0, 1, 1, 0x1908, 0x1401, (const void *)16); GLenum e_mapped = glGetError(); glUnmapBuffer(0x88EC); pxs(0, 0, a);
    CHECK(e_over == INVALID_OPERATION && e_mapped == INVALID_OPERATION && px_is(0, 0, 0, 255, 0, 255), "偏移 40 + 64 > 96 → INVALID_OPERATION（%#x）；映射着上传 → INVALID_OPERATION（%#x）；纹理不变 (0,0) %s", e_over, e_mapped, a);
    glBindBuffer(0x88EC, 0); const uint8_t blue[4] = {0, 0, 255, 255}; glTexSubImage2D(0x0DE1, 0, 0, 0, 1, 1, 0x1908, 0x1401, blue); GLenum e_cli = glGetError(); pxs(0, 0, a);
    CHECK(e_cli == 0 && px_is(0, 0, 0, 0, 255, 255), "解绑后 glTexSubImage2D 读客户端内存：(0,0) %s（期望 0 0 255 255，错 %#x）", a, e_cli);
    glDeleteBuffers(1, &ub); glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb); glDeleteTextures(1, &tex);
}

// ---------- O GLSL ES 3.00 程序 ----------
static void t_es300(void) {
    printf("O GLSL ES 3.00 程序\n");
    char v[] = "api_es300.vert", f[] = "api_es300.frag"; GLuint p = glCreateProgram(); glAttachShader(p, shader(0x8B31, v)); glAttachShader(p, shader(0x8B30, f)); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, 0x8B82, &ok); CHECK(ok == 1, "#version 300 es 程序在离线库里、链接成功"); if (!ok) return;
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    GLuint saved = g_tri; g_tri = p; const float q[] = {QUAD(-1, -1, 0, 1, 1, 0, 0)}; tri_use(q, 6); glDrawArrays(4, 0, 6); glFinish(); g_tri = saved;
    char a[32], b[32]; pxs(16, 32, a); pxs(48, 32, b);
    CHECK(px_is(16, 32, 255, 0, 0, 255) && px_is(48, 32, 0, 0, 255, 255) && !glGetError(), "左半红（%s）、右半清除色蓝（%s）", a, b);
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb); glDeleteProgram(p);
}

// ---------- P 采样器数组 ----------
static void t_sampler_array(void) {
    printf("P 采样器数组\n");
    G(glUniform1iv, void, GLint, GLsizei, const GLint *) G(glGetActiveUniform, void, GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)
    char v[] = "api_samparr.vert", f[] = "api_samparr.frag"; GLuint p = glCreateProgram(); glAttachShader(p, shader(0x8B31, v)); glAttachShader(p, shader(0x8B30, f)); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, 0x8B82, &ok); CHECK(ok == 1, "sampler2D tex[2] 程序在离线库里、链接成功"); if (!ok) return;
    GLuint tex[2]; glGenTextures(2, tex); const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    glActiveTexture(0x84C0 + 3); glBindTexture(0x0DE1, tex[0]); glTexImage2D(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, red); glTexParameteri(0x0DE1, 0x2801, 0x2600);
    glActiveTexture(0x84C0 + 5); glBindTexture(0x0DE1, tex[1]); glTexImage2D(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, green); glTexParameteri(0x0DE1, 0x2801, 0x2600);
    glActiveTexture(0x84C0);
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    GLuint saved = g_tri; g_tri = p; const float q[] = {QUAD(-1, -1, 1, 1, 1, 1, 1)}; tri_use(q, 6);
    GLint l0 = glGetUniformLocation(p, "tex"), l00 = glGetUniformLocation(p, "tex[0]"), l1 = glGetUniformLocation(p, "tex[1]"), l2 = glGetUniformLocation(p, "tex[2]");
    CHECK(l0 >= 0 && l00 == l0 && l1 == l0 + 1 && l2 == -1, "位置：tex = tex[0] = %d、tex[1] = %d（期望 +1）、tex[2] = %d（越界 −1）", l0, l1, l2);
    const GLint units[2] = {3, 5}; glUniform1iv(l0, 2, units); glDrawArrays(4, 0, 6); glFinish();
    char a[32], b[32]; pxs(16, 32, a); pxs(48, 32, b);
    CHECK(px_is(16, 32, 255, 0, 0, 255) && px_is(48, 32, 0, 255, 0, 255) && !glGetError(), "glUniform1iv 设两个单元：左 tex[0] 红（%s）、右 tex[1] 绿（%s）", a, b);
    glUniform1i(l1, 3); glDrawArrays(4, 0, 6); glFinish(); pxs(48, 32, b);
    CHECK(px_is(48, 32, 255, 0, 0, 255) && !glGetError(), "单设 tex[1] 为单元 3：右半变红（%s）", b);
    GLint nact = 0; glGetProgramiv(p, 0x8B86, &nact); char nm[32] = {0}; GLint sz = 0; GLenum ty = 0; GLsizei ln = 0; glGetActiveUniform(p, (GLuint)nact - 1, sizeof nm, &ln, &sz, &ty, nm);
    CHECK(!strcmp(nm, "tex[0]") && sz == 2 && ty == 0x8B5E, "活动 uniform：最后一个是「%s」大小 %d 类型 %#x（期望 tex[0] / 2 / SAMPLER_2D）", nm, sz, ty);
    g_tri = saved; glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb); glDeleteTextures(2, tex); glDeleteProgram(p);
}

// ---------- Q glGetShaderiv ----------
static void t_shader_iv(void) {
    printf("Q glGetShaderiv\n");
    G(glDetachShader, void, GLuint, GLuint) G(glIsShader, GLboolean, GLuint) G(glGetShaderInfoLog, void, GLuint, GLsizei, GLsizei *, GLchar *)
    char vf[] = "tri.vert"; GLuint vs = shader(0x8B31, vf); char *src = slurp(vf); GLint want_len = (GLint)strlen(src) + 1; free(src);
    GLint ty = 0, del = -1, comp = 0, log = -1, sl = 0; glGetShaderiv(vs, 0x8B4F, &ty); glGetShaderiv(vs, 0x8B80, &del); glGetShaderiv(vs, 0x8B81, &comp); glGetShaderiv(vs, 0x8B84, &log); glGetShaderiv(vs, 0x8B88, &sl);
    CHECK(ty == 0x8B31 && del == 0 && comp == 1 && log == 0 && sl == want_len && !glGetError(), "编过的顶点着色器：类型 %#x、删除 %d、编译 %d、日志长 %d、源码长 %d（期望 %d）", ty, del, comp, log, sl, want_len);
    GLuint bad = glCreateShader(0x8B30); GLint e0 = 0; glGetShaderiv(bad, 0x8B88, &e0);
    const GLchar *junk = "#version 320 es\nvoid main() {}\n// not in the offline library\n"; glShaderSource(bad, 1, &junk, NULL); glCompileShader(bad);
    GLint bc = 1, bl = 0, bt = 0; char buf[64]; GLsizei got = -1; glGetShaderiv(bad, 0x8B81, &bc); glGetShaderiv(bad, 0x8B84, &bl); glGetShaderiv(bad, 0x8B4F, &bt); glGetShaderInfoLog(bad, sizeof buf, &got, buf);
    CHECK(e0 == 0 && bc == 0 && bl == 41 && got == 40 && bt == 0x8B30 && !glGetError(), "没给源码时源码长 %d；不在库里：编译 %d、日志长 %d（期望 41）、读回 %d 字、类型 %#x", e0, bc, bl, got, bt);
    GLint junk_iv = 7; glGetShaderiv(vs, 0x1234, &junk_iv); CHECK(glGetError() == 0x0500 && junk_iv == 7, "未知 pname → INVALID_ENUM、不写输出");
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glDeleteShader(vs); GLint d1 = 0; glGetShaderiv(vs, 0x8B80, &d1); GLboolean alive = glIsShader(vs);
    glDetachShader(p, vs); GLboolean gone = !glIsShader(vs);
    CHECK(d1 == 1 && alive && gone && !glGetError(), "挂着时删：名字仍有效、删除状态 %d；分离后真删（glIsShader 假 %d）", d1, gone);
    glDeleteShader(bad); glDeleteProgram(p);
}

// ---------- R LÖVE 引擎的桌面上下文（Balatro） ----------
static void t_love(void) {
    printf("R LÖVE 引擎的桌面上下文\n");
    G(eglBindAPI, EGLBoolean, EGLenum) G(eglCreateContext, EGLContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *)
    G(eglMakeCurrent, EGLBoolean, EGLDisplay, EGLSurface, EGLSurface, EGLContext) G(eglGetCurrentContext, EGLContext, void) G(eglDestroyContext, EGLBoolean, EGLDisplay, EGLContext)
    G(glVertexAttrib4fv, void, GLuint, const GLfloat *) G(glVertexAttrib4f, void, GLuint, GLfloat, GLfloat, GLfloat, GLfloat) G(glBufferStorage, void, GLenum, GLsizeiptr, const void *, GLbitfield)
    G(glMapBufferRange, void *, GLenum, GLintptr, GLsizeiptr, GLbitfield) G(glUnmapBuffer, GLboolean, GLenum) G(glDrawElementsBaseVertex, void, GLenum, GLsizei, GLenum, const void *, GLint)
    G(glClearBufferfv, void, GLenum, GLint, const GLfloat *) G(glGetUniformfv, void, GLuint, GLint, GLfloat *) G(glPointSize, void, GLfloat) G(glHint, void, GLenum, GLenum)
    G(glInvalidateFramebuffer, void, GLenum, GLsizei, const GLenum *) G(glDrawArraysInstanced, void, GLenum, GLint, GLsizei, GLsizei)
    EGLContext es = eglGetCurrentContext(); eglBindAPI(0x30A2); EGLContext gl = eglCreateContext(g_dpy, g_cfg, es, NULL); eglBindAPI(0x30A0);
    if (!gl || !eglMakeCurrent(g_dpy, g_sf, g_sf, gl)) { CHECK(0, "桌面上下文建立（共享 ES 上下文）"); return; }
    while (glGetError()) {}
    GLint sb = -1, sm = -1, prof = -1, m3 = -1, layers = -1; GLfloat pr[2] = {-1, -1};
    glGetIntegerv(0x80A8, &sb); glGetIntegerv(0x80A9, &sm); glGetIntegerv(0x9126, &prof); glGetIntegerv(0x8073, &m3); glGetIntegerv(0x88FF, &layers); glGetFloatv(0x0B12, pr);
    CHECK(sb == 0 && sm == 0 && prof == 1 && m3 == 2048 && layers == 2048 && pr[0] == 1 && pr[1] == 511 && !glGetError(),
          "initMaxValues 的查询：SAMPLE_BUFFERS %d、SAMPLES %d、PROFILE_MASK %d、MAX_3D %d、ARRAY_LAYERS %d、POINT_SIZE_RANGE [%g, %g]，不置错", sb, sm, prof, m3, layers, pr[0], pr[1]);
    const GLfloat white[4] = {1, 1, 1, 1}; glVertexAttrib4fv(2, white); glVertexAttrib4fv(3, white);   // setupContext 的头两笔（ATTRIB_COLOR / CONSTANTCOLOR）
    CHECK(!glGetError(), "setupContext 头两笔 glVertexAttrib4fv 不置错");
    GLuint fb = fbo_rb(64, 64, NULL); glClearColor(0, 0, 1, 1); glClear(0x4000);
    const float left[] = {QUAD(-1, -1, 0, 1, 1, 0, 0)}; tri_use(left, 6);
    GLint ac = glGetAttribLocation(g_tri, "aCol"); glDisableVertexAttribArray((GLuint)ac); glVertexAttrib4f((GLuint)ac, 0, 1, 0, 1); glDrawArrays(4, 0, 6); glFinish();
    char a[32], b[32]; pxs(16, 32, a); CHECK(px_is(16, 32, 0, 255, 0, 255) && !glGetError(), "glVertexAttrib4f 设关掉数组的 aCol = 绿：左半绿（%s）", a);
    // 流式缓冲：不可变存储 + 持久一致映射，映射着画、接着写下一段再画
    GLuint sbo; glGenBuffers(1, &sbo); glBindBuffer(0x8892, sbo); const GLsizeiptr sz = 12 * 5 * sizeof(float);
    glBufferStorage(0x8892, sz, NULL, 0xC2); float *mp = glMapBufferRange(0x8892, 0, sz, 0xC2);
    CHECK(mp && !glGetError(), "glBufferStorage(WRITE | PERSISTENT | COHERENT) + 同位持久映射：拿到指针、不置错");
    if (mp) {
        const float q2[] = {QUAD(-1, -1, 0, 1, 1, 0, 0), QUAD(0, -1, 1, 1, 0, 1, 0)};
        GLint ap = glGetAttribLocation(g_tri, "aPos");
        glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 20, (void *)0); glEnableVertexAttribArray((GLuint)ap);
        glVertexAttribPointer((GLuint)ac, 3, 0x1406, 0, 20, (void *)8); glEnableVertexAttribArray((GLuint)ac);
        glClear(0x4000); memcpy(mp, q2, 30 * sizeof(float)); glDrawArrays(4, 0, 6); GLenum e1 = glGetError();
        memcpy(mp + 30, q2 + 30, 30 * sizeof(float)); glDrawArrays(4, 6, 6); GLenum e2 = glGetError(); glFinish();
        pxs(16, 32, a); pxs(48, 32, b);
        CHECK(!e1 && !e2 && px_is(16, 32, 255, 0, 0, 255) && px_is(48, 32, 0, 255, 0, 255), "映射着画第一段、接着写第二段再画：错误 %#x / %#x，左红（%s）右绿（%s）", e1, e2, a, b);
        glBufferData(0x8892, 16, NULL, 0x88E4); GLenum ebd = glGetError(); const float z = 0; glBufferSubData(0x8892, 0, 4, &z); GLenum esd = glGetError();
        CHECK(ebd == INVALID_OPERATION && esd == INVALID_OPERATION, "不可变存储上 BufferData %#x、没有 DYNAMIC_STORAGE 的 SubData %#x（期望都 INVALID_OPERATION）", ebd, esd);
        // 基准顶点：索引 0..5、基准 6 → 画第二段
        GLuint ibo; glGenBuffers(1, &ibo); glBindBuffer(0x8893, ibo); const uint16_t ix[6] = {0, 1, 2, 3, 4, 5}; glBufferData(0x8893, sizeof ix, ix, 0x88E4);
        glClear(0x4000); glDrawElementsBaseVertex(4, 6, 0x1403, 0, 6); glFinish(); pxs(16, 32, a); pxs(48, 32, b);
        CHECK(px_is(16, 32, 0, 0, 255, 255) && px_is(48, 32, 0, 255, 0, 255) && !glGetError(), "glDrawElementsBaseVertex 基准 6：左清除色蓝（%s）、右第二段绿（%s）", a, b);
        glBindBuffer(0x8893, 0); glDeleteBuffers(1, &ibo);
        glClear(0x4000); glDrawArraysInstanced(4, 0, 6, 1); glFinish(); pxs(16, 32, a);
        CHECK(px_is(16, 32, 255, 0, 0, 255) && !glGetError(), "glDrawArraysInstanced 1 个实例：左半红（%s）", a);
        glUnmapBuffer(0x8892);
    }
    GLuint mb; glGenBuffers(1, &mb); glBindBuffer(0x8892, mb); glBufferData(0x8892, 64, NULL, 0x88E8); void *bad = glMapBufferRange(0x8892, 0, 64, 0x42);
    CHECK(!bad && glGetError() == INVALID_OPERATION, "可变存储（BufferData 建的）上持久映射 → 空指针、INVALID_OPERATION");
    glDeleteBuffers(1, &mb); glDeleteBuffers(1, &sbo);
    const GLfloat yellow[4] = {1, 1, 0, 1}; glClearBufferfv(0x1800, 0, yellow); glFinish(); GLfloat cc[4] = {0}; glGetFloatv(0x0C22, cc); pxs(16, 32, a);
    CHECK(px_is(16, 32, 255, 255, 0, 255) && cc[0] == 0 && cc[1] == 0 && cc[2] == 1 && !glGetError(), "glClearBufferfv(COLOR, 0, 黄)：像素黄（%s）、COLOR_CLEAR_VALUE 仍是蓝 (%g %g %g)", a, cc[0], cc[1], cc[2]);
    GLint ut = glGetUniformLocation(g_tri, "uTint"); glUseProgram(g_tri); glUniform4f(ut, 0.25f, 0.5f, 0.75f, 1); GLfloat rv[4] = {0}; glGetUniformfv(g_tri, ut, rv);
    CHECK(rv[0] == 0.25f && rv[1] == 0.5f && rv[2] == 0.75f && rv[3] == 1 && !glGetError(), "glGetUniformfv 读回 uTint (%g %g %g %g)", rv[0], rv[1], rv[2], rv[3]);
    glPointSize(2); GLfloat ps = 0; glGetFloatv(0x0B11, &ps); glPointSize(0); GLenum eps = glGetError();
    CHECK(ps == 2 && eps == INVALID_VALUE, "glPointSize(2) → POINT_SIZE %g；glPointSize(0) → %#x（期望 INVALID_VALUE）", ps, eps);
    const GLenum att[2] = {0x8D00, 0x8D20}; glHint(0x8192, 0x1102); glInvalidateFramebuffer(0x8D40, 2, att); GLenum eh = glGetError(); glInvalidateFramebuffer(0x1234, 2, att);
    CHECK(!eh && glGetError() == INVALID_ENUM, "glHint / glInvalidateFramebuffer 不置错；坏目标 → INVALID_ENUM");
    glUniform4f(ut, 1, 1, 1, 1);
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &fb);
    eglMakeCurrent(g_dpy, g_sf, g_sf, es); eglDestroyContext(g_dpy, gl);
}

// S 核心函数不回空（小丑牌 v0.2.6 iPhone 起局 5 次闪退：LÖVE 按报的 GL 4.5 取核心函数，没实现的回空，调用跳到 0 被代码签名杀掉）；
//   GL_FRAMEBUFFER_SRGB 开关可开关可查、不置错（原先 glIsEnabled / glDisable 都置 0x500）。期望按 GL 规范与本层约定手写：
//   核心名（结尾不是两个以上大写字母）没实现 → 非空、调用回 0；带厂商后缀的没实现 → 空；FRAMEBUFFER_SRGB 缺省关、开了查得到开、全程无错
//   #601 起空实现按调用来源给：查表 gl_lookup 只回真实现（装载器先问直连层，层里交了空实现，游戏模块自己导出的同名函数就轮不到）；
//   游戏运行时按名要（eglGetProcAddress）查表没有时才给空实现。所以下面空实现各项从 eglGetProcAddress 取，查表另核回空
static void t_core_stub(void) {
    void *(*egpa)(const char *) = gpa("eglGetProcAddress"); CHECK(egpa != NULL, "eglGetProcAddress 查得到"); if (!egpa) return;
    CHECK(gpa("glS2aNoSuchCore") == NULL, "查表（gl_lookup）对没实现的核心名回空（空实现不从查表给，#601）");
    void *core = egpa("glS2aNoSuchCore"), *ext = egpa("glS2aNoSuchFuncARB");
    CHECK(core != NULL, "没实现的核心名拿到非空入口（空实现）");
    CHECK(core && ((uint64_t (*)(void))core)() == 0, "调用空实现不崩、回 0");
    CHECK(core == egpa("glS2aNoSuchCore"), "同名再查拿到同一个空实现");
    CHECK(ext == NULL, "带厂商后缀的没实现照旧回空（程序按指针判扩展支持）");
    // #601：「gl」后跟小写的不是 GL 函数（NVN GLSL 编译库 glslc*，由游戏模块导出）——必须回空，装载器才会去游戏模块找
    { static const char *const glslc[] = {"glslcInitialize", "glslcSetAllocator", "glslcCompile", "glslcFinalize", "glslcGetVersion", "glslcCompilePreSpecialized"};
      int nonnull = 0; for (int i = 0; i < 6; i++) nonnull += (gpa(glslc[i]) != NULL) + (egpa(glslc[i]) != NULL);
      CHECK(nonnull == 0, "glslc* 六个名字全部回空（交给游戏模块），拿到非空 %d 个", nonnull); }
    // 小丑牌 v0.2.7 起局查了 854 个没实现的核心名：1000 个不同名字都要拿到各自的空实现（以前 256 个用完就回空、照样跳 0）
    int got = 0, distinct = 1; void *prev = NULL; char nm[40];
    for (int i = 0; i < 1000; i++) { snprintf(nm, sizeof nm, "glS2aMissCore%dxyz", i); void *f = egpa(nm); got += f != NULL; if (f && f == prev) distinct = 0; prev = f; }
    CHECK(got == 1000 && distinct, "1000 个不同的没实现核心名全部拿到空实现、相邻两个不共用（拿到 %d）", got);
    // 编号空实现用完以后（再查 3200 个，累计超 4096）照样不回空，拿到的共用空实现调用回 0
    int got2 = 0, ret0 = 1; for (int i = 0; i < 3200; i++) { snprintf(nm, sizeof nm, "glS2aMissMore%dxyz", i); void *f = egpa(nm); got2 += f != NULL; if (f && ((uint64_t (*)(void))f)() != 0) ret0 = 0; }
    CHECK(got2 == 3200 && ret0, "超过 4096 个没实现核心名后仍全部拿到空实现、调用回 0（拿到 %d）", got2);
    GLboolean (*isen)(GLenum) = gpa("glIsEnabled"); void (*en)(GLenum) = gpa("glEnable"), (*dis)(GLenum) = gpa("glDisable");
    while (glGetError()) {}
    GLboolean a = isen(0x8DB9); GLenum e0 = glGetError(); en(0x8DB9); GLboolean b = isen(0x8DB9); GLenum e1 = glGetError(); dis(0x8DB9); GLboolean c = isen(0x8DB9); GLenum e2 = glGetError();
    CHECK(!a && b && !c && !e0 && !e1 && !e2, "GL_FRAMEBUFFER_SRGB：缺省关 / 开后查得开 / 关后查得关，全程无错（%d %d %d，错 %#x %#x %#x）", a, b, c, e0, e1, e2);
    for (int k = 0; k < 2; k++) { GLenum cap = k ? 0x8242 : 0x92E0;   // GL_DEBUG_OUTPUT / _SYNCHRONOUS：只记开关、不报错（小丑牌 glDisable(GL_DEBUG_OUTPUT)）
        dis(cap); GLboolean x = isen(cap); GLenum f0 = glGetError(); en(cap); GLboolean y = isen(cap); GLenum f1 = glGetError(); dis(cap); GLenum f2 = glGetError();
        CHECK(!x && y && !f0 && !f1 && !f2, "%s：关后查得关 / 开后查得开，全程无错（%d %d，错 %#x %#x %#x）", k ? "GL_DEBUG_OUTPUT_SYNCHRONOUS" : "GL_DEBUG_OUTPUT", x, y, f0, f1, f2); }
}
// LÖVE 11 起局 createDefaultTexture：3D 与 2D 数组各建 1×1 不可变存储、子图写一个白像素（小丑牌 v0.2.7 崩在这里：glTexStorage3D 以前回空）
static void t_tex_storage3d(void) {
    void (*st3)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei) = gpa("glTexStorage3D");
    void (*sub3)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, uint64_t, uint64_t, const void *) = gpa("glTexSubImage3D");
    void (*img3)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, uint64_t, const void *) = gpa("glTexImage3D");
    void (*getimg)(GLenum, GLint, GLenum, GLenum, void *) = gpa("glGetTexImage"); void (*gtpi)(GLenum, GLenum, GLint *) = gpa("glGetTexParameteriv");
    if (!st3 || !sub3 || !img3 || !getimg) return;
    while (glGetError()) {}
    const uint8_t white[4] = {255, 255, 255, 255}; uint8_t out[4]; GLuint t[2]; glGenTextures(2, t); const GLenum tg[2] = {0x806F, 0x8C1A};   // 3D、2D 数组
    for (int i = 0; i < 2; i++) {
        glActiveTexture(0x84C0); glBindTexture(tg[i], t[i]); st3(tg[i], 1, 0x8058, 1, 1, 1); GLenum e0 = glGetError();
        sub3(tg[i], 0, 0, 0, 0, 1, 1, 1, 0x1908, 0x1401, white); GLenum e1 = glGetError();
        memset(out, 0, sizeof out); getimg(tg[i], 0, 0x1908, 0x1401, out); GLenum e2 = glGetError();
        GLint lv = -1; if (gtpi) gtpi(tg[i], 0x82DF, &lv);
        CHECK(!e0 && !e1 && !e2 && !memcmp(out, white, 4) && lv == 1, "%s 1×1 不可变存储 + 子图写白 → 读回白、IMMUTABLE_LEVELS=1、无错（错 %#x %#x %#x，读回 %d %d %d %d，级数 %d）",
              i ? "2D 数组" : "3D", e0, e1, e2, out[0], out[1], out[2], out[3], lv);
        st3(tg[i], 1, 0x8058, 1, 1, 1); GLenum e3 = glGetError(); img3(tg[i], 0, 0x8058, 1, 1, 1, 0, 0x1908, 0x1401, white); GLenum e4 = glGetError();
        CHECK(e3 == 0x502 && e4 == 0x502, "%s 不可变后再 TexStorage3D / TexImage3D 都报 INVALID_OPERATION（%#x %#x）", i ? "2D 数组" : "3D", e3, e4);
    }
    {   // 游戏按 8 字节一格放栈上参数、高 32 位是脏的（小丑牌实测 type 读成 0x70）：宿主只取低 32 位
        GLuint tg9; glGenTextures(1, &tg9); glBindTexture(0x806F, tg9); st3(0x806F, 1, 0x8058, 1, 1, 1); while (glGetError()) {}
        sub3(0x806F, 0, 0, 0, 0, 1, 1, 1, 0x0000007000001908ull, 0x0000007000001401ull, white); GLenum eg = glGetError(); memset(out, 0, sizeof out); getimg(0x806F, 0, 0x1908, 0x1401, out);
        CHECK(!eg && !memcmp(out, white, 4), "glTexSubImage3D 栈上 format / type 高 32 位脏：只取低位、写入无错、读回白（错 %#x，读回 %d %d %d %d）", eg, out[0], out[1], out[2], out[3]);
    }
    GLuint t3; glGenTextures(1, &t3); glBindTexture(0x806F, t3); st3(0x806F, 3, 0x8058, 4, 4, 2); GLenum e5 = glGetError();
    uint8_t px[4 * 4 * 2 * 4]; memset(px, 0x40, sizeof px); sub3(0x806F, 2, 0, 0, 0, 1, 1, 1, 0x1908, 0x1401, white); GLenum e6 = glGetError();
    sub3(0x806F, 2, 0, 0, 0, 1, 1, 2, 0x1908, 0x1401, px); GLenum e7 = glGetError();
    CHECK(!e5 && !e6 && e7 == 0x501, "3D 4×4×2 三级：第 2 级是 1×1×1（写 1 片无错、写 2 片越界 INVALID_VALUE；错 %#x %#x %#x）", e5, e6, e7);
    st3(0x0DE1, 1, 0x8058, 1, 1, 1); GLenum e8 = glGetError(); CHECK(e8 == 0x500, "2D 目标给 TexStorage3D 报 INVALID_ENUM（%#x）", e8);
    glBindTexture(0x806F, 0); glBindTexture(0x8C1A, 0);
}
// GL_TEXTURE_SWIZZLE_*（小丑牌 / LÖVE 11 建单 / 双通道纹理时设；以前报 INVALID_ENUM、LÖVE 抛异常退出）。期望手算：texel (10,20,30,40)，
//   换位 R←B、G←R、B←1、A←G → 采样 (30,10,255,20)；改回缺省 → (10,20,30,40)；采样器对象设它 → INVALID_ENUM
static void t_tex_swizzle(void) {
    printf("S2 纹理通道换位\n");
    G(glSamplerParameteri, void, GLuint, GLenum, GLint) G(glGenSamplers, void, GLsizei, GLuint *) G(glGetTexParameteriv, void, GLenum, GLenum, GLint *)
    GLuint prog = program("texture"), fb = fbo_rb(8, 8, NULL); (void)fb; glUseProgram(prog); glViewport(0, 0, 8, 8);
    glUniform2f(glGetUniformLocation(prog, "uScale"), 1, 1); glUniform2f(glGetUniformLocation(prog, "uOff"), 0, 0);
    glUniform2f(glGetUniformLocation(prog, "uUVScale"), 1, 1); glUniform2f(glGetUniformLocation(prog, "uUVOff"), 0, 0); glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}; glBufferData(0x8892, sizeof quad, quad, 0x88E4);
    GLint ap = glGetAttribLocation(prog, "aPos"); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 8, 0); glEnableVertexAttribArray((GLuint)ap);
    const uint8_t px[4] = {10, 20, 30, 40}; GLuint tex; glGenTextures(1, &tex); glActiveTexture(0x84C0); glBindTexture(0x0DE1, tex);
    glTexParameteri(0x0DE1, 0x2801, 0x2600); glTexParameteri(0x0DE1, 0x2800, 0x2600); glTexImage2D(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, px);
    while (glGetError()) {}
    glTexParameteri(0x0DE1, 0x8E42, 0x1905); glTexParameteri(0x0DE1, 0x8E43, 0x1903); glTexParameteri(0x0DE1, 0x8E44, 1); glTexParameteri(0x0DE1, 0x8E45, 0x1904); GLenum e0 = glGetError();
    GLint q[4] = {0}; for (int c = 0; c < 4; c++) glGetTexParameteriv(0x0DE1, 0x8E42 + c, &q[c]);
    CHECK(!e0 && q[0] == 0x1905 && q[1] == 0x1903 && q[2] == 1 && q[3] == 0x1904, "SWIZZLE_R/G/B/A 设置无错、查询读回（错 %#x，读回 %#x %#x %#x %#x）", e0, q[0], q[1], q[2], q[3]);
    char s[32]; glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
    CHECK(px_is(4, 4, 30, 10, 255, 20), "换位后采样 = (30,10,255,20)（%s）", s);
    for (int c = 0; c < 4; c++) glTexParameteri(0x0DE1, 0x8E42 + c, 0x1903 + c);
    glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
    CHECK(px_is(4, 4, 10, 20, 30, 40) && !glGetError(), "改回缺省后采样 = 原值 (10,20,30,40)（%s）", s);
    GLuint smp; glGenSamplers(1, &smp); glSamplerParameteri(smp, 0x8E42, 0x1903); GLenum e1 = glGetError(); glTexParameteri(0x0DE1, 0x8E42, 0x1234); GLenum e2 = glGetError();
    CHECK(e1 == 0x500 && e2 == 0x500, "采样器对象设换位 / 非法取值 → INVALID_ENUM（%#x %#x）", e1, e2);
    glBindTexture(0x0DE1, 0);
}
// bool uniform（小丑牌 / LÖVE：着色器头 #define extern uniform，Balatro 写 extern bool；SPIR-V 默认块里 bool 换成 uint，反射报 GL_UNSIGNED_INT 时
//   LÖVE Shader:send 拒收布尔值、Lua 报错退出）。期望：类型报 GL_BOOL / GL_BOOL_VEC2；uFlag=1 → 绿，uPair=(0,1) → 蓝分量满 → (0,255,255)；uFlag=0、uPair=0 → 红
static void t_bool_uniform(void) {
    printf("S3 bool uniform\n");
    G(glGetActiveUniform, void, GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *) G(glUniform2iv, void, GLint, GLsizei, const GLint *)
    GLuint prog = program("api_bool"), fb = fbo_rb(8, 8, NULL); (void)fb; glUseProgram(prog); glViewport(0, 0, 8, 8);
    GLenum tf = 0, tp = 0; GLint sz; char nm[64];
    GLint n = 0; glGetProgramiv(prog, 0x8B86, &n);
    for (GLint i = 0; i < n; i++) { GLenum t = 0; glGetActiveUniform(prog, (GLuint)i, sizeof nm, NULL, &sz, &t, nm); if (!strcmp(nm, "uFlag")) tf = t; if (!strcmp(nm, "uPair")) tp = t; }
    CHECK(tf == 0x8B56 && tp == 0x8B57, "extern bool / bvec2 的类型报 GL_BOOL / GL_BOOL_VEC2（%#x %#x）", tf, tp);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(0x8892, vbo);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}; glBufferData(0x8892, sizeof quad, quad, 0x88E4);
    GLint ap = glGetAttribLocation(prog, "aPos"); glVertexAttribPointer((GLuint)ap, 2, 0x1406, 0, 8, 0); glEnableVertexAttribArray((GLuint)ap);
    GLint lf = glGetUniformLocation(prog, "uFlag"), lp = glGetUniformLocation(prog, "uPair"); char s[32];
    glUniform1i(lf, 1); const GLint p01[2] = {0, 1}, p00[2] = {0, 0}; glUniform2iv(lp, 1, p01); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
    CHECK(px_is(4, 4, 0, 255, 255, 255) && !glGetError(), "uFlag=1、uPair=(0,1) → (0,255,255)（%s）", s);
    glUniform1i(lf, 0); glUniform2iv(lp, 1, p00); glDrawArrays(4, 0, 6); glFinish(); pxs(4, 4, s);
    CHECK(px_is(4, 4, 255, 0, 0, 255) && !glGetError(), "uFlag=0、uPair=(0,0) → 红（%s）", s);
}
int main(void) { alarm(120); @autoreleasepool {
#define LOAD(name, ret, ...) name = (ret (*)(__VA_ARGS__))gpa(#name); if (!name) { printf("✗ 取不到已有入口 %s\n", #name); return 1; }
    BASE(LOAD)
    EGLDisplay (*eglGetDisplay)(void *) = gpa("eglGetDisplay"); EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = gpa("eglInitialize");
    EGLBoolean (*eglBindAPI)(EGLenum) = gpa("eglBindAPI"); EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = gpa("eglChooseConfig");
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *) = gpa("eglCreateWindowSurface");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = gpa("eglCreateContext");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = gpa("eglMakeCurrent");
    eglBindAPI(0x30A0); const EGLint cfg_attr[] = {0x3040, 0x0040, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038}, ctx_attr[] = {0x3098, 3, 0x30FB, 2, 0x3038};
    g_dpy = eglGetDisplay(NULL); EGLint ma, mi; eglInitialize(g_dpy, &ma, &mi); EGLConfig cfg; EGLint n; eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n); g_cfg = cfg;
    g_sf = eglCreateWindowSurface(g_dpy, cfg, NULL, NULL); EGLContext ctx = eglCreateContext(g_dpy, cfg, NULL, ctx_attr);
    if (!eglMakeCurrent(g_dpy, g_sf, g_sf, ctx)) { printf("✗ 建上下文失败\n"); return 1; }
    g_tri = program("tri"); glGenVertexArrays(1, &g_tri_vao); glGenBuffers(1, &g_tri_vbo);
    t_queries(); t_sync(); t_readback(); t_fb_query(); t_volume(); t_msaa(); t_map(); t_instanced(); t_packed_attr(); t_raster_read(); t_uniforms(); t_names_programs(); t_threads(); t_frame_storage(); t_core_stub(); t_tex_storage3d(); t_tex_swizzle(); t_bool_uniform(); t_api_version(); t_egl_current(); t_map_range(); t_pack_buffer(); t_unpack_buffer(); t_es300(); t_sampler_array(); t_shader_iv(); t_love();
    printf("%s t_gl_api：%d 项，失败 %d\n", fails ? "✗" : "✓", checks, fails); return fails != 0;
}}
