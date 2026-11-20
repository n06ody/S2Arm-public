// 缓冲与顶点数组（分片「缓冲与顶点数组」，#272）。最小集：Gen / Bind / Delete Buffers、BufferData / SubData、顶点数组与属性指针。
// 缓冲 = 共享存储 MTLBuffer；BufferData 换新存储（GL 语义：旧内容丢弃，GPU 还在读的旧存储由已编码的命令持有、读完即释放，等同驱动的「孤立」）。
#include "gl_int.h"
const char gl_slice_gl_buf = 1;   // 分片锚（gl_core.m 引用：静态链接时整片进来，见 gl_int.h「分片锚」）

enum { GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8A11, GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502, GL_OUT_OF_MEMORY = 0x0505 };
#define CTX(ret) do { if (!gl_cur) { GLRED1("%s：没有当前上下文", __func__); return ret; } } while (0)

// 绑定目标 → gl_state.buf_bound 的序号（元素数组缓冲绑在顶点数组上，不在这里）
//   像素打包缓冲（GL 2.1 / ES 3.0）：绑着它时 glReadPixels 的 data 是缓冲内偏移、读回写进缓冲（gl_fb.m）。D-WR8B Worms W.M.D：进战役时
//   绑它 → glBufferStorage → glMapBufferRange 拿指针、两条工作线程往里写，原先三步都报「目标没实现」回空，线程写 0 / 0x800 崩。
//   像素解包缓冲（0x88EC，GL 2.1 / ES 3.0）：绑着它时纹理上传（glTex(Sub)Image2D/3D、glCompressedTex(Sub)Image2D）的 data 是缓冲内偏移、像素从缓冲取（gl_tex.m）。
//   D-W2MJ 白日梦的构想图（#1440）：绑 0x88EC → glMapBufferRange 写像素 → 从缓冲上传纹理，原先绑定 / 映射都报「目标没实现」，纹理一张没上，整屏黑
static int buf_slot(GLenum target) { return target == GL_ARRAY_BUFFER ? 0 : target == GL_UNIFORM_BUFFER ? 1 : target == GL_PIXEL_PACK_BUFFER ? 2 : target == GL_PIXEL_UNPACK_BUFFER ? 3 : -1; }
struct gl_vao *gl_vao_cur(void) {
    struct gl_vao *v = gl_name_get(&gl_vaos, gl_cur->vao);
    if (!v && !gl_cur->vao) {   // 顶点数组 0：兼容配置 / GLES 的缺省顶点数组，属于当前上下文（core 配置下用它是错误，但 GLES 3.x 允许）
        return &gl_cur->default_vao;
    }
    return v;
}
static void buffer_destroy(void *object) {
    struct gl_buffer *b = object; b->buf = nil; b->idx16 = nil; b->fan32_cache = nil; free(b);
}
void gl_buf_session_reset(void) {
    gl_names_clear(&gl_buffers, buffer_destroy); gl_names_clear(&gl_vaos, free);
}
GL(glGenBuffers, void, GLsizei n, GLuint *out) { CTX(); for (GLsizei i = 0; i < n; i++) out[i] = gl_name_new(&gl_buffers, calloc(1, sizeof(struct gl_buffer))); }
GL(glDeleteBuffers, void, GLsizei n, const GLuint *names) {
    CTX(); for (GLsizei i = 0; i < n; i++) { struct gl_buffer *b = gl_name_del(&gl_buffers, names[i]); if (b) buffer_destroy(b); }
}
GL(glBindBuffer, void, GLenum target, GLuint name) {
    CTX();
    if (name && !gl_name_get(&gl_buffers, name)) { struct gl_buffer *nb = calloc(1, sizeof *nb); if (!nb || !gl_name_put(&gl_buffers, name, nb)) { free(nb); gl_set_error(GL_OUT_OF_MEMORY); return; } }   // 没 Gen 过的名字：GLES 允许 Bind 时建
    int k = target == GL_ELEMENT_ARRAY_BUFFER ? 0 : buf_slot(target); if (k < 0) { GLRED1("glBindBuffer 目标 %#x 没实现", target); gl_set_error(GL_INVALID_ENUM); return; }
    if (name) ((struct gl_buffer *)gl_name_get(&gl_buffers, name))->used = 1;   // 第一次绑定：名字成为缓冲对象（glIsBuffer）
    if (target == GL_ELEMENT_ARRAY_BUFFER) { struct gl_vao *v = gl_vao_cur(); if (v) v->ebo = name; return; }
    gl_cur->buf_bound[k] = name;
}
// 编号绑定点（GLES 3.0 统一缓冲）：整块绑到第 index 个绑定点，同时设为 GL_UNIFORM_BUFFER 的通用绑定（GL 规定的副作用）。绘制时按程序命名块的绑定点取（gl_draw.m）
GL(glBindBufferBase, void, GLenum target, GLuint index, GLuint name) {
    CTX();
    if (target != GL_UNIFORM_BUFFER) { GLRED1("glBindBufferBase 目标 %#x 没实现（只有 GL_UNIFORM_BUFFER）", target); gl_set_error(GL_INVALID_ENUM); return; }
    if (index >= 32) { gl_set_error(GL_INVALID_VALUE); return; }
    if (name && !gl_name_get(&gl_buffers, name)) { struct gl_buffer *nb = calloc(1, sizeof *nb); if (!nb || !gl_name_put(&gl_buffers, name, nb)) { free(nb); gl_set_error(GL_OUT_OF_MEMORY); return; } }
    if (name) ((struct gl_buffer *)gl_name_get(&gl_buffers, name))->used = 1;
    gl_cur->ubo[index] = name; gl_cur->ubo_off[index] = 0; gl_cur->ubo_size[index] = 0;   // size 0 = 整块
    gl_cur->buf_bound[buf_slot(GL_UNIFORM_BUFFER)] = name;
}
static struct gl_buffer *bound(GLenum target) {
    GLuint name = target == GL_ELEMENT_ARRAY_BUFFER ? (gl_vao_cur() ? gl_vao_cur()->ebo : 0) : buf_slot(target) >= 0 ? gl_cur->buf_bound[buf_slot(target)] : 0;
    return gl_name_get(&gl_buffers, name);
}
GLuint gl_pack_buffer_name(void) { return gl_cur->buf_bound[buf_slot(GL_PIXEL_PACK_BUFFER)]; }
struct gl_buffer *gl_pack_buffer(void) { return bound(GL_PIXEL_PACK_BUFFER); }
GLuint gl_unpack_buffer_name(void) { return gl_cur->buf_bound[buf_slot(GL_PIXEL_UNPACK_BUFFER)]; }
struct gl_buffer *gl_unpack_buffer(void) { return bound(GL_PIXEL_UNPACK_BUFFER); }
GLB(glBufferData, void, GLenum target, GLsizeiptr size, const void *data, GLenum usage) { @autoreleasepool {
    (void)usage; CTX();
    struct gl_buffer *b = bound(target); if (!b || b->storage) { gl_set_error(GL_INVALID_OPERATION); return; }   // 不可变存储不许 BufferData（GL 4.5 §6.2）
    if (size < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_dev_init();
    @autoreleasepool {
        size_t n = size ? (size_t)size : 16;
        id<MTLBuffer> next = [gl_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!next) { GLOG("【红】 glBufferData 存储分配失败（%zu 字节）", n); gl_set_error(GL_OUT_OF_MEMORY); return; }
        if (data && size) memcpy(next.contents, data, (size_t)size);
        b->buf = next; b->idx16 = nil; b->fan32_cache = nil; b->size = (size_t)size; b->last_use = 0;
        b->map_access = 0;   // 规范：对映射着的缓冲 BufferData 先隐式解除映射（旧存储随之丢弃）
    }
}}
// 不可变存储（GL 4.4 / 4.5 §6.2，Balatro 所用的 LÖVE 引擎在 4.4 以上用它建流式缓冲：建一次、持久映射、每帧往里写、靠栅栏错开 GPU 在读的那段）。
//   存储同 glBufferData（共享 MTLBuffer），记下标志位：之后不许再 BufferData，没有 DYNAMIC_STORAGE 不许 SubData，映射位要在标志里（glMapBufferRange 核）
enum { GL_MAP_PERSISTENT_BIT = 0x40, GL_MAP_COHERENT_BIT = 0x80, GL_DYNAMIC_STORAGE_BIT = 0x100, GL_CLIENT_STORAGE_BIT = 0x200 };
GLB(glBufferStorage, void, GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) { @autoreleasepool {
    CTX();
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { GLRED1("glBufferStorage 目标 %#x 没实现", target); gl_set_error(GL_INVALID_ENUM); return; }
    if (size <= 0 || (flags & ~(GLbitfield)0x3C3)) { gl_set_error(GL_INVALID_VALUE); return; }   // 只认 MAP_READ / WRITE / PERSISTENT / COHERENT / DYNAMIC_STORAGE / CLIENT_STORAGE
    if (((flags & GL_MAP_PERSISTENT_BIT) && !(flags & 3)) || ((flags & GL_MAP_COHERENT_BIT) && !(flags & GL_MAP_PERSISTENT_BIT))) { gl_set_error(GL_INVALID_VALUE); return; }
    struct gl_buffer *b = bound(target); if (!b || b->storage) { gl_set_error(GL_INVALID_OPERATION); return; }
    gl_glBufferData(target, size, data, 0);
    if (b->buf) b->storage = flags | 0x80000000u;   // 高位：建过（flags 可以是 0）
}}
// 当前前端只有 CPU 写缓冲：在途存储的 CPU 内容仍是最新值。复制后换存储，保留此前绘制读到的内容，不切渲染遍。
GLB(glBufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, const void *data) { @autoreleasepool {
    CTX();
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { GLRED1("glBufferSubData 目标 %#x 没实现", target); gl_set_error(GL_INVALID_ENUM); return; }
    struct gl_buffer *b = bound(target);
    if (!b || !b->buf || (b->map_access && !(b->map_bits & GL_MAP_PERSISTENT_BIT)) || (b->storage && !(b->storage & GL_DYNAMIC_STORAGE_BIT))) { gl_set_error(GL_INVALID_OPERATION); return; }   // 映射着（持久映射除外）/ 不可变且没有 DYNAMIC_STORAGE 不许 SubData（规范）
    if (off < 0 || size < 0 || (size_t)off > b->size || (size_t)size > b->size - (size_t)off) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!size) return;
    @autoreleasepool {
        if (gl_buffer_busy(b) && !b->map_access) {   // 持久映射着：游戏拿着的指针指向这份存储，不能换（游戏自己用栅栏错开）
            id<MTLBuffer> next = [gl_dev newBufferWithLength:b->size options:MTLResourceStorageModeShared];
            if (!next) { GLOG("【红】 glBufferSubData 存储分配失败（%zu 字节）", b->size); gl_set_error(GL_OUT_OF_MEMORY); return; }
            memcpy(next.contents, b->buf.contents, b->size);
            memcpy((uint8_t *)next.contents + off, data, (size_t)size);
            b->buf = next; b->last_use = 0;   // 已编码的命令继续持有旧存储；新存储还没被引用。
        } else {
            memcpy((uint8_t *)b->buf.contents + off, data, (size_t)size);
        }
        b->idx16 = nil; b->fan32_cache = nil;   // 原地写和换存储都生成新内容，整表失效；已编码绘制仍持有旧副本。
    }
}}
GL(glGenVertexArrays, void, GLsizei n, GLuint *out) { CTX(); for (GLsizei i = 0; i < n; i++) out[i] = gl_name_new(&gl_vaos, calloc(1, sizeof(struct gl_vao))); }
GL(glDeleteVertexArrays, void, GLsizei n, const GLuint *names) { CTX(); for (GLsizei i = 0; i < n; i++) if (names[i]) free(gl_name_del(&gl_vaos, names[i])); }
GL(glBindVertexArray, void, GLuint name) {
    CTX(); if (name && !gl_name_get(&gl_vaos, name)) { gl_set_error(GL_INVALID_OPERATION); return; }
    gl_cur->vao = name; gl_cur->dirty |= GLD_PIPE | GLD_VB;
}
GL(glVertexAttribPointer, void, GLuint i, GLint size, GLenum type, GLuint norm, GLsizei stride, const void *ptr) {
    CTX(); struct gl_vao *v = gl_vao_cur();
    if (i >= 16 || size < 1 || size > 4 || stride < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!v) { gl_set_error(GL_INVALID_OPERATION); return; }
    // 没绑数组缓冲 = 客户端内存顶点数组（buf 0，off 是游戏内存地址）：绘制时按本笔读到的顶点范围拷进临时缓冲（gl_draw.m setup）
    gl_attrib *a = &v->a[i]; a->buf = gl_cur->buf_bound[0]; a->size = size; a->type = type; a->norm = norm & 0xff; a->stride = stride; a->off = (GLintptr)ptr;
    gl_cur->dirty |= GLD_PIPE | GLD_VB;
}
GL(glEnableVertexAttribArray, void, GLuint i) { CTX(); struct gl_vao *v = gl_vao_cur(); if (i >= 16 || !v) { gl_set_error(GL_INVALID_VALUE); return; } v->a[i].enabled = 1; gl_cur->dirty |= GLD_PIPE | GLD_VB; }
// DSA 版本：直接改指定顶点数组（0 = 缺省顶点数组）
GL(glEnableVertexArrayAttrib, void, GLuint vao, GLuint i) {
    CTX(); struct gl_vao *v = vao ? gl_name_get(&gl_vaos, vao) : gl_vao_cur(); if (!v) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (i >= 16) { gl_set_error(GL_INVALID_VALUE); return; } v->a[i].enabled = 1; gl_cur->dirty |= GLD_PIPE | GLD_VB;
}
GL(glDisableVertexArrayAttrib, void, GLuint vao, GLuint i) {
    CTX(); struct gl_vao *v = vao ? gl_name_get(&gl_vaos, vao) : gl_vao_cur(); if (!v) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (i >= 16) { gl_set_error(GL_INVALID_VALUE); return; } v->a[i].enabled = 0; gl_cur->dirty |= GLD_PIPE | GLD_VB;
}
GL(glDisableVertexAttribArray, void, GLuint i) { CTX(); struct gl_vao *v = gl_vao_cur(); if (i >= 16 || !v) { gl_set_error(GL_INVALID_VALUE); return; } v->a[i].enabled = 0; gl_cur->dirty |= GLD_PIPE | GLD_VB; }
// glIsBuffer：名字在表里、且绑定过（Gen 出、没绑过的名字还不是缓冲对象：GL 4.5 §6.1 / ES 3.2 §6.1）
GL(glIsBuffer, GLboolean, GLuint name) { CTX(0); struct gl_buffer *b = gl_name_get(&gl_buffers, name); return b && b->used; }
// 映射（桌面 GL glMapBuffer，Slay the Spire）：缓冲是共享存储，CPU 直接读写 contents。GL 的顺序语义：映射后的写不能影响此前已发出、GPU 还没执行完的绘制——
//   可写映射遇到在途存储时同 glBufferSubData：先复制成新存储再交出新存储的指针（已编码命令持有旧存储），不等 GPU；只读映射直接交出当前内容（CPU 写是唯一写者，内容就是最新）
enum { GL_READ_ONLY = 0x88B8, GL_WRITE_ONLY = 0x88B9, GL_READ_WRITE = 0x88BA };
GLB(glMapBuffer, void *, GLenum target, GLenum access) { @autoreleasepool {
    CTX(NULL);
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { GLRED1("glMapBuffer 目标 %#x 没实现", target); gl_set_error(GL_INVALID_ENUM); return NULL; }
    if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) { gl_set_error(GL_INVALID_ENUM); return NULL; }
    struct gl_buffer *b = bound(target);
    if (!b || !b->buf || b->map_access) { gl_set_error(GL_INVALID_OPERATION); return NULL; }   // 没绑 / 没存储 / 已经映射着
    if (access != GL_READ_ONLY && gl_buffer_busy(b)) { @autoreleasepool {
        id<MTLBuffer> next = [gl_dev newBufferWithLength:b->buf.length options:MTLResourceStorageModeShared];
        if (!next) { GLOG("【红】 glMapBuffer 存储分配失败（%zu 字节）", (size_t)b->buf.length); gl_set_error(GL_OUT_OF_MEMORY); return NULL; }
        memcpy(next.contents, b->buf.contents, b->buf.length); b->buf = next; b->last_use = 0;
    } }
    b->map_access = access; b->map_bits = 0; b->map_off = 0; b->map_len = b->size; return b->buf.contents;
}}
// 范围映射（GLES 3.0 核心 / GL 4.5 §6.3）：同 glMapBuffer 交出共享存储的指针（加偏移）。火影羁绊 Scaleform 按 ES 3.x 取这个入口写顶点，原先查表回空，
//   它拿到空指针往 0 写（main+0xca9fd8 崩）。
//   同步语义：没带 UNSYNCHRONIZED 的可写映射遇在途存储先换新存储（同 glMapBuffer）；INVALIDATE_BUFFER 时旧内容可丢、不复制；带 UNSYNCHRONIZED 由游戏自己保证不冲突，直接交当前存储
enum { GL_MAP_READ_BIT = 1, GL_MAP_WRITE_BIT = 2, GL_MAP_INVALIDATE_RANGE_BIT = 4, GL_MAP_INVALIDATE_BUFFER_BIT = 8, GL_MAP_FLUSH_EXPLICIT_BIT = 0x10, GL_MAP_UNSYNCHRONIZED_BIT = 0x20 };
GLB(glMapBufferRange, void *, GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) { @autoreleasepool {
    CTX(NULL);
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { GLRED1("glMapBufferRange 目标 %#x 没实现", target); gl_set_error(GL_INVALID_ENUM); return NULL; }
    struct gl_buffer *b = bound(target); if (!b) { gl_set_error(GL_INVALID_OPERATION); return NULL; }
    if (off < 0 || len <= 0 || (size_t)off > b->size || (size_t)len > b->size - (size_t)off || (access & ~(GLbitfield)0xFF)) { gl_set_error(GL_INVALID_VALUE); return NULL; }
    int rd = access & GL_MAP_READ_BIT, wr = access & GL_MAP_WRITE_BIT, pers = access & GL_MAP_PERSISTENT_BIT;
    if (!b->buf || b->map_access || (!rd && !wr) || (rd && (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) ||
        ((access & GL_MAP_FLUSH_EXPLICIT_BIT) && !wr)) { gl_set_error(GL_INVALID_OPERATION); return NULL; }
    // 持久 / 一致映射只对不可变存储、且建存储时带了同样的位（读写位也要在里面）；一致映射要求持久（GL 4.5 §6.3）
    if ((access & (GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT)) && (!b->storage || (access & (GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | 3) & ~b->storage) || ((access & GL_MAP_COHERENT_BIT) && !pers))) { gl_set_error(GL_INVALID_OPERATION); return NULL; }
    if (b->storage && (access & 3 & ~b->storage)) { gl_set_error(GL_INVALID_OPERATION); return NULL; }
    // 持久映射交出的指针游戏一直拿着写：不换存储（换了它写的就是旧存储，绘制读不到）；它跟 GPU 的错开由游戏自己的栅栏保证（规范要求）
    if (wr && !pers && !(access & GL_MAP_UNSYNCHRONIZED_BIT) && gl_buffer_busy(b)) { @autoreleasepool {
        id<MTLBuffer> next = [gl_dev newBufferWithLength:b->buf.length options:MTLResourceStorageModeShared];
        if (!next) { GLOG("【红】 glMapBufferRange 存储分配失败（%zu 字节）", (size_t)b->buf.length); gl_set_error(GL_OUT_OF_MEMORY); return NULL; }
        if (!(access & GL_MAP_INVALIDATE_BUFFER_BIT)) memcpy(next.contents, b->buf.contents, b->buf.length);
        b->buf = next; b->last_use = 0;
    } }
    b->map_access = wr ? (rd ? GL_READ_WRITE : GL_WRITE_ONLY) : GL_READ_ONLY; b->map_bits = access; b->map_off = (size_t)off; b->map_len = (size_t)len;
    return (uint8_t *)b->buf.contents + off;
}}
// 显式刷新：范围相对映射起点。共享存储没有要搬的数据，只核参数（GLES 3.0 §2.10.3 / GL 4.5 §6.3.1 的错误语义）
GL(glFlushMappedBufferRange, void, GLenum target, GLintptr off, GLsizeiptr len) {
    CTX();
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { gl_set_error(GL_INVALID_ENUM); return; }
    struct gl_buffer *b = bound(target); if (!b) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (off < 0 || len < 0 || (size_t)off > b->map_len || (size_t)len > b->map_len - (size_t)off) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!b->map_access || !(b->map_bits & GL_MAP_FLUSH_EXPLICIT_BIT)) { gl_set_error(GL_INVALID_OPERATION); return; }
}
GLB(glUnmapBuffer, GLboolean, GLenum target) { @autoreleasepool {
    CTX(0);
    if (target != GL_ELEMENT_ARRAY_BUFFER && buf_slot(target) < 0) { gl_set_error(GL_INVALID_ENUM); return 0; }
    struct gl_buffer *b = bound(target); if (!b || !b->map_access) { gl_set_error(GL_INVALID_OPERATION); return 0; }
    if (b->map_access != GL_READ_ONLY) { b->idx16 = nil; b->fan32_cache = nil; }   // 经映射写过：索引副本按新内容重建
    b->map_access = 0; b->map_bits = 0; b->map_off = b->map_len = 0; return 1;   // 共享存储不会丢内容，恒 GL_TRUE
}}
// 实例步进：divisor 0 = 逐顶点，N = 每 N 个实例前进一个元素（绘制时进顶点描述的 PerInstance / stepRate，gl_draw.m）
GL(glVertexAttribDivisor, void, GLuint i, GLuint divisor) {
    CTX(); struct gl_vao *v = gl_vao_cur(); if (i >= 16) { gl_set_error(GL_INVALID_VALUE); return; } if (!v) { gl_set_error(GL_INVALID_OPERATION); return; }
    v->a[i].divisor = divisor; gl_cur->dirty |= GLD_PIPE | GLD_VB;
}
// 当前通用顶点属性（上下文状态，不属于顶点数组）：数组关掉的属性读它（gl_draw.m setup）
//   没给的分量按规范补 (0, 0, 1)：glVertexAttrib4fv 是 Balatro（LÖVE 引擎）建上下文后的第一笔调用，原先查表回空，游戏跳到地址 0 闪退
static void attr_cur_set(GLuint i, int n, const GLfloat *v) {
    if (i >= 16) { gl_set_error(GL_INVALID_VALUE); return; }
    static const GLfloat def[4] = {0, 0, 0, 1};
    for (int k = 0; k < 4; k++) gl_cur->attr_cur[i][k] = k < n ? v[k] : def[k];
    gl_cur->dirty |= GLD_VB;
}
GL(glVertexAttrib1f, void, GLuint i, GLfloat x) { CTX(); attr_cur_set(i, 1, &x); }
GL(glVertexAttrib2f, void, GLuint i, GLfloat x, GLfloat y) { CTX(); GLfloat v[2] = {x, y}; attr_cur_set(i, 2, v); }
GL(glVertexAttrib3f, void, GLuint i, GLfloat x, GLfloat y, GLfloat z) { CTX(); GLfloat v[3] = {x, y, z}; attr_cur_set(i, 3, v); }
GL(glVertexAttrib4f, void, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { CTX(); GLfloat v[4] = {x, y, z, w}; attr_cur_set(i, 4, v); }
GL(glVertexAttrib1fv, void, GLuint i, const GLfloat *v) { CTX(); attr_cur_set(i, 1, v); }
GL(glVertexAttrib2fv, void, GLuint i, const GLfloat *v) { CTX(); attr_cur_set(i, 2, v); }
GL(glVertexAttrib3fv, void, GLuint i, const GLfloat *v) { CTX(); attr_cur_set(i, 3, v); }
GL(glVertexAttrib4fv, void, GLuint i, const GLfloat *v) { CTX(); attr_cur_set(i, 4, v); }

// ---------- 即时存档：缓冲（内容整块存，共享存储直接读）与顶点数组 ----------
// 映射着的缓冲（持久映射：LÖVE 引擎建一次、一直拿着指针往里写）：游戏内存里存着 MTLBuffer 内容的宿主地址。存档时按「gl:map:<名字>:<偏移>:<长度>」登记
//   （gl_snap_handout_now，快照引擎写交出指针表之前调），恢复时快照引擎按身份要新地址：这里先按长度建出存储（gl_buf_snap_map），之后恢复内容时沿用这块
enum { GB_NAME = 1, GB_SIZE, GB_LEN, GB_MAPACC, GB_USED, GB_MAPBITS, GB_MAPOFF, GB_MAPLEN, GB_STORAGE, GV_NAME = 1, GV_ATTR, GV_EBO };
void s2a_snap_handout_id(uint64_t v, const char *id) __attribute__((weak));
void gl_buf_snap_handout(void) {
    if (!s2a_snap_handout_id) return; int n = 0;
    for (GLuint i = 1; i < gl_buffers.n; i++) { struct gl_buffer *b = gl_buffers.v[i]; if (!b || !b->map_access || !b->buf) continue;
        char id[96]; snprintf(id, sizeof id, "gl:map:%u:%zu:%lu", i, b->map_off, (unsigned long)b->buf.length);
        s2a_snap_handout_id((uint64_t)(uintptr_t)((uint8_t *)b->buf.contents + b->map_off), id); n++; }
    if (n) GLOG("即时存档：映射着的缓冲 %d 个（指针按身份登记）", n);
}
static struct gl_buffer *buf_shell(GLuint name, size_t len) {   // 恢复：按名字取 / 建缓冲对象，存储按长度建（已有同长存储就沿用）
    struct gl_buffer *b = gl_name_get(&gl_buffers, name);
    if (!b) { b = calloc(1, sizeof *b); if (!b || !gl_name_put(&gl_buffers, name, b)) { free(b); return NULL; } }
    if (len && (!b->buf || b->buf.length != len)) { gl_dev_init(); b->buf = [gl_dev newBufferWithLength:len options:MTLResourceStorageModeShared]; if (!b->buf) return NULL; }
    return b;
}
uint64_t gl_buf_snap_map(const char *id) {   // "<名字>:<偏移>:<长度>"
    unsigned nm = 0; size_t off = 0; unsigned long len = 0; if (sscanf(id, "%u:%zu:%lu", &nm, &off, &len) != 3 || !nm || off > len) return 0;
    struct gl_buffer *b = buf_shell(nm, len); return b && b->buf ? (uint64_t)(uintptr_t)((uint8_t *)b->buf.contents + off) : 0;
}
int gl_buf_snap_save(FILE *f) {
    sfw w; sfw_sec(&w, f, "gl.buf", 1); uint32_t n = 0;
    for (GLuint i = 1; i < gl_buffers.n; i++) { struct gl_buffer *b = gl_buffers.v[i]; if (!b) continue;
        uint64_t len = b->buf ? b->buf.length : 0;
        sfw_rec(&w); GLSF_W(&w, GB_NAME, i); GLSF_W(&w, GB_SIZE, b->size); GLSF_W(&w, GB_LEN, len); GLSF_W(&w, GB_MAPACC, b->map_access); GLSF_W(&w, GB_USED, b->used);
        GLSF_W(&w, GB_MAPBITS, b->map_bits); GLSF_W(&w, GB_MAPOFF, b->map_off); GLSF_W(&w, GB_MAPLEN, b->map_len); GLSF_W(&w, GB_STORAGE, b->storage); sfw_end_rec(&w); n++; }
    if (sfw_end(&w)) return -1;
    // 内容：按名字顺序紧跟（长度见上面的记录）
    uint64_t bytes = 0;
    for (GLuint i = 1; i < gl_buffers.n; i++) { struct gl_buffer *b = gl_buffers.v[i]; if (!b || !b->buf) continue;
        if (fwrite(b->buf.contents, 1, b->buf.length, f) != b->buf.length) return -1; bytes += b->buf.length; }
    sfw_sec(&w, f, "gl.vao", 1);
    for (GLuint i = 1; i < gl_vaos.n; i++) { struct gl_vao *v = gl_vaos.v[i]; if (!v) continue; sfw_rec(&w); GLSF_W(&w, GV_NAME, i); GLSF_W(&w, GV_ATTR, v->a); GLSF_W(&w, GV_EBO, v->ebo); sfw_end_rec(&w); }
    GLOG("即时存档：GL 缓冲 %u 个 %.1f MB", n, bytes / 1048576.0);
    return sfw_end(&w);
}
int gl_buf_snap_restore(FILE *f) {
    sfr r; if (sfr_sec(&r, f, "gl.buf", 1)) return -1;
    GLuint *names = NULL; uint64_t *lens = NULL; uint32_t n = 0, cap = 0; int bad = 0;
    while (sfr_rec(&r) == 1) {
        GLuint nm = 0; uint64_t len = 0; GLSF_R(&r, GB_NAME, nm); GLSF_R(&r, GB_LEN, len); if (!nm) continue;
        struct gl_buffer *b = buf_shell(nm, (size_t)len); if (!b) { bad = 1; break; }
        GLSF_R(&r, GB_SIZE, b->size); GLSF_R(&r, GB_MAPACC, b->map_access); GLSF_R(&r, GB_USED, b->used); GLSF_R(&r, GB_MAPBITS, b->map_bits);
        GLSF_R(&r, GB_MAPOFF, b->map_off); GLSF_R(&r, GB_MAPLEN, b->map_len); GLSF_R(&r, GB_STORAGE, b->storage);
        b->idx16 = nil; b->fan32_cache = nil; b->last_use = 0;
        if (len) { if (n == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, cap * sizeof *names); lens = realloc(lens, cap * sizeof *lens); } names[n] = nm; lens[n++] = len; }
    }
    sfr_done(&r);
    for (uint32_t i = 0; !bad && i < n; i++) { struct gl_buffer *b = gl_name_get(&gl_buffers, names[i]); if (fread(b->buf.contents, 1, (size_t)lens[i], f) != lens[i]) bad = 1; }
    free(names); free(lens);
    if (bad || sfr_sec(&r, f, "gl.vao", 1)) return -1;
    while (sfr_rec(&r) == 1) { GLuint nm = 0; GLSF_R(&r, GV_NAME, nm); struct gl_vao *v = calloc(1, sizeof *v); if (!nm || !v) { free(v); continue; }
        GLSF_RC(&r, GV_ATTR, v->a, "gl.attrib", gl_attrib); GLSF_R(&r, GV_EBO, v->ebo); if (!gl_name_put(&gl_vaos, nm, v)) { free(v); bad = 1; } }
    sfr_done(&r);
    return bad ? -1 : 0;
}
