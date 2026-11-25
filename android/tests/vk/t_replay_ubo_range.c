// SPDX-License-Identifier: Apache-2.0
// 常量缓冲长于着色器声明（64 KiB）且长于设备 maxUniformBufferRange：真实 rp_draw + rp_buf + Vulkan 后端，读回像素。
#include "rp_exec.h"
#include "replay_pass_barrier_fixture.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <time.h>
static nvn_gpu_err err;
static atomic_int retired;
static unsigned failures;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){(void)n;return d;}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static void done(void *u,const nvn_gpu_done *d){(void)u;(void)d;}
static void retirement(void *u){(void)u;atomic_fetch_add(&retired,1);}
void rp_buf_reset(void);
static nvn_gpu_buf buffer(uint64_t bytes,const void *data){nvn_gpu_buf b=NULL;GPU(nvn_gpu_buf_create(bytes,NVN_GPU_MEM_SHARED,data,&b,&err));return b;}
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout){
 char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
 FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
 void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
 nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
 nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
// 程序与池登记用固定夹具。缓冲解析、回放绑定与 Vulkan 后端使用生产实现。
int g_s2a_logfd=2,s2a_nvn_rast_flip;
enum { CBUF_BYTES = 0x10000, UBO_OFF = 0x1000, GUEST_BYTES = UBO_OFF + 0x100000 };
static alignas(4096) unsigned char guest[GUEST_BYTES];
static struct rp_prog prog;static p_set_rt target;static tex_t target_tex;
static nvn_gpu_view target_view;static nvn_gpu_cmd active;
static uint64_t serial=1;static unsigned emitted;static int pass_open;
void __real_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a){emitted++;__real_nvn_gpu_draw(c,a);}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&prog;return 0;}
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return 1;}
uint64_t rp_frame(void){return 1;}   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身，同 t_replay_indirect）
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return 1;}
int rp_fence_done(uint64_t f){(void)f;return 1;}
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_flush(active);*last=serial;return active;}
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
int rp_post_gpu_write_from(uint64_t a,uint64_t n, struct rp_write_source source) { (void)source; (void)a;(void)n;return 0;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
// 夹具池不跟踪脏页，走常规缓冲同步。
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return 1;}
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return 1;}
const void *nvn_dirty_span(const void *a,size_t n){(void)a;(void)n;return NULL;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;(void)g;(void)n;return -1;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)i;(void)c;if (t) memset(t, 0, sizeof *t);return NULL;}
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { (void)t; (void)v; (void)c; return NULL; }
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)i;(void)t;(void)k;(void)e;(void)c;if (o) memset(o, 0, sizeof *o);return NULL;}
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
void rp_decl_probe_begin(void){} int rp_decl_probe_end(void){return 0;}
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid){(void)tid;return rp_samp_by_id(sid);}
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn){(void)pb;(void)pn;return rp_samp_for_tex(sid,tid);}   // 同 rp_samp_for_tex：替身不给默认采样器
// 夹具不提供查询采样器；请求该能力时返回不可用。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f){return nvn_gpu_format_caps(f);}
int rp_tex_snap_by_id(int i,tex_t *t,view_t *v,int *h){(void)i;(void)t;(void)v;(void)h;return 0;}
uint64_t rp_tex_reg_gen(void){return 1;}uint64_t rp_tex_fact_sig(const uint64_t *h,int n,int b,int c){(void)h;(void)n;(void)b;(void)c;return 0;}
void rp_tex_mark_gpu_written(const tex_t *t){(void)t;}int rp_tex_debug_state(uint64_t s){(void)s;return 0;}
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s){(void)s;return NULL;}
const p_set_rt *rp_pass_target(void){return &target;}int rp_pass_slot_attached(int i){return i==0;}
void rp_pass_slot_size(int i,uint32_t *w,uint32_t *h){(void)i;*w=*h=64;}void rp_pass_noatt_size(uint32_t *w,uint32_t *h){(void)w;(void)h;}void rp_pass_note_draw(int d){(void)d;}
// #950 给 rp_draw.c 新加的通道依赖：中性替身（没有待折清除、不缩渲染区域）
int rp_pass_parked(void){return 0;}void rp_pass_prepare_draw(nvn_gpu_cmd c,uint32_t color_write_mask){(void)c;(void)color_write_mask;} void rp_pass_want_layered(nvn_gpu_cmd c, int layered) { (void)c; (void)layered; }void rp_pass_land_reads(nvn_gpu_cmd c,const rp_pass_read *ts,int n){(void)c;(void)ts;(void)n;}
static void pass(nvn_gpu_cmd c,int clear){nvn_gpu_pass_desc p={0};p.ncolor=1;p.color[0].view=target_view;p.color[0].load=clear?NVN_GPU_LOAD_CLEAR:NVN_GPU_LOAD_LOAD;p.color[0].store=NVN_GPU_STORE_STORE;p.color[0].clear.f[3]=1;p.w=p.h=64;p.layers=1;nvn_gpu_pass_begin(c,&p);pass_open=1;}
int rp_pass_begin(nvn_gpu_cmd c){if(!pass_open)pass(c,0);return 1;}void rp_pass_flush(nvn_gpu_cmd c){if(pass_open){nvn_gpu_pass_end(c);pass_open=0;}}
// 片元着色器读取常量缓冲声明的最后 16 字节。两臂仅改变游戏绑定长度。
static void run_case(struct s2a_vk_device *device, const char *dir, uint64_t size) {
    nvn_gpu_open_desc open = {.host_device = device, .app_label = "real-replay-ubo-range"};
    GPU(nvn_gpu_open(&open, &err));
    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    REQUIRE(caps.max_ubo_range == device->properties.limits.maxUniformBufferRange);

    memset(&prog, 0, sizeof prog);
    prog.nent[1] = 1;
    nvn_gpu_layout_ent *ent = &prog.ent[1][0];
    *ent = (nvn_gpu_layout_ent){
        .set = 0, .binding = 1, .cls = NVN_GPU_RES_UBO, .src_kind = NVN_RB_CBUF,
        .src_reg = 0, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_FS
    };
    nvn_gpu_layout layout = {.ent = ent, .nent = 1};
    prog.sh[0] = shader(dir, "quad.vert", NVN_GPU_SH_VS, NULL);
    prog.sh[1] = shader(dir, "replay_ubo_range.frag", NVN_GPU_SH_FS, &layout);
    prog.attr_mask = 3;
    prog.color_out_mask = 15;

    memset(guest, 0, sizeof guest);
    float (*vertices)[8] = (float (*)[8])(void *)guest;
    static const float corners[6][2] = {{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
    for (unsigned i = 0; i < 6; i++) {
        vertices[i][0] = corners[i][0];
        vertices[i][1] = corners[i][1];
        vertices[i][3] = vertices[i][7] = 1;
    }
    memset(guest + UBO_OFF, 0xEE, (size_t)size);
    const uint32_t color[4] = {17, 34, 51, 255};
    memcpy(guest + UBO_OFF + CBUF_BYTES - sizeof color, color, sizeof color);

    nvn_gpu_tex_desc td = {0};
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 64;
    td.d.depth = td.d.levels = 1;
    td.usage = NVN_GPU_TEX_RENDER;
    nvn_gpu_tex tex = NULL;
    GPU(nvn_gpu_tex_create(&td, &tex, &err));
    GPU(nvn_gpu_view_create(tex, NULL, &target_view, &err));
    nvn_gpu_buf readback = buffer(64 * 64 * 4, NULL);
    target_tex = (tex_t){.d = td.d, .serial = 1};
    target = (p_set_rt){.n = 1, .color = {&target_tex}};

    rp_state_reset();
    rp_draw_reset();
    nvn_unimpl_reset();
    rp_S.nattr = 2;
    rp_S.nstream = 1;
    rp_S.attr[0].fmt = rp_S.attr[1].fmt = NVN_FORMAT_RGBA32F;
    rp_S.attr[1].offset = 16;
    rp_S.stream[0].stride = 32;
    rp_S.vbuf[0].address = (uint64_t)(uintptr_t)guest;
    rp_S.vbuf[0].size = 192;
    rp_S.color.logic_op = 3;
    rp_S.chmask.mask = 15;
    rp_S.poly.mode = NVN_POLYGON_MODE_FILL;
    rp_S.ubo[1][0].addr = (uint64_t)(uintptr_t)(guest + UBO_OFF);
    rp_S.ubo[1][0].size = size;

    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    serial++;
    pass(active, 1);
    rp_pass_flush(active);
    const unsigned before = emitted;
    p_draw draw = {.prim = 4, .count = 6, .instances = 1};
    rp_draw(active, &draw, NULL, &draw);
    rp_pass_flush(active);
    nvn_gpu_copy_tex_buf(active, tex, 0, 0, NULL, readback, 0, 256, 16384);

    GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    const uint8_t *pixels = nvn_gpu_buf_cpu(readback);
    const uint8_t expected[4] = {17, 34, 51, 255};
    unsigned bad = 0;
    for (unsigned i = 0; i < 64 * 64; i++) {
        if (memcmp(pixels + i * 4, expected, sizeof expected)) bad++;
    }
    printf("CBUF size=%llu draws=%u pixels=4096 bad=%u unimpl=%d\n",
           (unsigned long long)size, emitted - before, bad, nvn_unimpl_kinds());
    REQUIRE(emitted == before + 1);
    REQUIRE(nvn_unimpl_kinds() == 0);
    failures += bad;

    rp_draw_reset();
    rp_buf_reset();
    nvn_gpu_shader_release(prog.sh[0]);
    nvn_gpu_shader_release(prog.sh[1]);
    nvn_gpu_view_release(target_view);
    nvn_gpu_tex_release(tex);
    const int target_retired = atomic_load(&retired) + 1;
    nvn_gpu_buf_release(readback, retirement, NULL);
    const struct timespec pause = {.tv_nsec = 1000000};
    for (int i = 0; i < 5000 && atomic_load(&retired) < target_retired; i++) nanosleep(&pause, NULL);
    REQUIRE(atomic_load(&retired) == target_retired);
    GPU(nvn_gpu_close(5000000000ull, &err));
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct s2a_vk_device device;
    char why[256];
    if (s2a_vk_device_create(&device, why, sizeof why)) {
        fprintf(stderr, "%s\n", why);
        return 2;
    }
    const uint64_t max_ubo = device.properties.limits.maxUniformBufferRange;
    printf("device: %s, maxUniformBufferRange=%llu\n", device.properties.deviceName, (unsigned long long)max_ubo);
    REQUIRE(max_ubo >= CBUF_BYTES && max_ubo + 0x8000 <= GUEST_BYTES - UBO_OFF);
    run_case(&device, argv[1], CBUF_BYTES);
    run_case(&device, argv[1], max_ubo + 0x8000);
    REQUIRE(!s2a_vk_device_destroy(&device, 1));
    return failures ? 1 : 0;
}

// 本夹具不模拟真实块代号；返回未知，让回放沿用逐页保守路径。
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
