// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Zero color masks: production rp_pass/rp_draw/rp_buf and the real Vulkan backend.
// Only resource registration and shader lookup are fixtures; selection is never mocked.
#include "rp_exec.h"
#include "rp_state.h"
#include "rp_res.h"
#include "rp_shader.h"
#include "nvn_unimpl.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "vk_device.h"
#include "nvn_plat_vk.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <time.h>
extern uint64_t nvn_vk_dbg_dq_recs(void);
static nvn_gpu_err err;
static unsigned failures, cases;
static int dq_mode;
static int pdc_enabled, pdc_scope, watch_pdc;
static unsigned pdc_loads, pdc_dont_care, pdc_cases, pdc_failures;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"assertion failed line %d: %s rc=%d why=%s\n",__LINE__,#x,err.rc.code,err.why);exit(2);} } while(0)
#define GPU(x) REQUIRE(nvn_gpu_ok(x))
const char *const *nvn_plat_vk_instance_exts(uint32_t *n){*n=0;return NULL;}
VkResult nvn_plat_vk_surface_create(VkInstance i,void *w,VkSurfaceKHR *s){(void)i;(void)w;*s=VK_NULL_HANDLE;return VK_ERROR_EXTENSION_NOT_PRESENT;}
int nvn_plat_window_size(void *w,uint32_t *x,uint32_t *y){(void)w;(void)x;(void)y;return 0;}
void nvn_plat_safepoint(void){}
int nvn_plat_flag(const char *n,int d){
    if (!strcmp(n, "nvk_dq")) return dq_mode;
    if (!strcmp(n, "nvk_pass_dont_care")) return pdc_enabled;
    if (!strcmp(n, "nvk_pass_discard_scope")) return pdc_scope && !getenv("S2A_TEST_PDC_WITHDRAW");
    if (!strcmp(n, "nvk_pass_cache") && pdc_enabled) return 0;
    return d;
}
void nvn_plat_log(int l,const char *c,const char *s){(void)l;fprintf(stderr,"[%s] %s\n",c?c:"",s?s:"");}
static void done(void *u,const nvn_gpu_done *d){(void)u;(void)d;}
void rp_buf_reset(void);
static nvn_gpu_buf buffer(uint64_t bytes,const void *data){nvn_gpu_buf b=NULL;GPU(nvn_gpu_buf_create(bytes,NVN_GPU_MEM_SHARED,data,&b,&err));return b;}
static nvn_gpu_shader shader(const char *dir,const char *name,nvn_gpu_stage stage,const nvn_gpu_layout *layout){
 char path[1024];REQUIRE(snprintf(path,sizeof path,"%s/%s.spv",dir,name)<(int)sizeof path);
 FILE *f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long n=ftell(f);REQUIRE(n>0);rewind(f);
 void *data=malloc((size_t)n);REQUIRE(data);REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
 nvn_gpu_shader_src s={0};s.stage=stage;s.kind=NVN_GPU_BLOB_SPIRV;s.entry="main";s.blob=data;s.bytes=(size_t)n;s.layout=layout;
 nvn_gpu_shader h=NULL;GPU(nvn_gpu_shader_load(&s,&h,&err));free(data);return h;
}
// A strict fixed registry supplies two color images, optional depth, and one guest pool.
int g_s2a_logfd=2,s2a_nvn_rast_flip;
static alignas(4096) unsigned char guest[16384];
static struct rp_prog prog;static nvn_gpu_cmd active;
static tex_t textures[3];static nvn_gpu_tex images[3];static nvn_gpu_view views[3];
static nvn_gpu_pass_desc passes[32];static unsigned npass;
void __real_nvn_gpu_pass_begin(nvn_gpu_cmd c,const nvn_gpu_pass_desc *p);
void __wrap_nvn_gpu_pass_begin(nvn_gpu_cmd c,const nvn_gpu_pass_desc *p){REQUIRE(npass<32);passes[npass++]=*p;__real_nvn_gpu_pass_begin(c,p);}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *,
                                                        const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci,
                                                         const VkAllocationCallbacks *a, VkRenderPass *out) {
    // The pipeline-compatibility pass uses DONT_CARE for load and store. The
    // actual pass in this fixture always loads both initialized attachments.
    if (watch_pdc) for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        if (ci->pAttachments[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) continue;
        pdc_loads++;
        pdc_dont_care += ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return __real_vkCreateRenderPass(d, ci, a, out);
}
static uint64_t serial=1;static unsigned emitted;static int completed;
void __real_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a);
void __wrap_nvn_gpu_draw(nvn_gpu_cmd c,const nvn_gpu_draw_args *a){emitted++;__real_nvn_gpu_draw(c,a);}
uint64_t nvn_ga2ca(uint64_t a){return a;}
int nvn_dpool_base(const void *p,int t){(void)p;(void)t;return 0;}
const prog_t *nvn_prog_snap_get(uint64_t s,uint8_t k[6][8],uint8_t *o,uint8_t t[6]){(void)s;(void)k;(void)o;(void)t;return NULL;}
int rp_shader_resolve(const struct rp_shader_req *q,const struct rp_prog **o){(void)q;*o=&prog;return 0;}
static int fixture_origin=1;
int rp_depth_zero(void){return 0;}int rp_origin_lower_left(void){return fixture_origin;}
uint64_t rp_frame(void) { return 1; }   // #996：rp_draw 首用策略按帧号判（rp_entry.c 的替身）
void rp_perf_add(int i,uint64_t n){(void)i;(void)n;}uint64_t rp_seg_serial(void){return serial;}
uint64_t rp_sub_serial(void){return serial;}uint64_t rp_sub_fence(void){return 1;}
int rp_fence_done(uint64_t f){return !f || completed;}
int rp_discard_submit_reject(nvn_gpu_cmd c) { return c == active ? 0 : RP_PERF_DC_NON_PRIMARY; }
nvn_gpu_cmd rp_xfer_cmd(uint64_t *last){rp_pass_flush(active);*last=serial;return active;}
void rp_occl_draw(nvn_gpu_cmd c){(void)c;}
int rp_occl_want(void){return 0;}void rp_occl_pass_opened(int d){REQUIRE(!d);}
void rp_occl_pass_closed(void){}void rp_occl_pause(nvn_gpu_cmd c){(void)c;}
static uint64_t write_key,write_id;
int rp_post_gpu_write(uint64_t a,uint64_t n){REQUIRE(!write_id);write_id=rp_buf_gpu_write_note(a,n,&write_key);REQUIRE(write_id);return 1;}
int rp_pool_find(uint64_t a,uint64_t n,const uint8_t **m,uint64_t *s){uint64_t base=(uint64_t)(uintptr_t)guest;if(a<base||n>sizeof guest||a-base>sizeof guest-n)return 0;if(m)*m=guest;if(s)*s=sizeof guest;return 1;}
// Deliberately untracked pool: use the replay core's conservative synchronization.
size_t nvn_dirty_page_size(void){return 4096;}uint64_t nvn_dirty_now(void){return 1;}
uint64_t nvn_dirty_mapping_generation(void){return 1;}int nvn_dirty_tracking_unreliable(void){return 1;}
const void *nvn_dirty_span(const void *a,size_t n){(void)a;(void)n;return NULL;}
long nvn_dirty_page_gens(const void *s,const void *a,size_t l,uint64_t *g,size_t n){(void)s;(void)a;(void)l;(void)g;(void)n;return -1;}
void nvn_dirty_protect(const void *a,size_t n){(void)a;(void)n;}
void rp_view_pin_begin(void){}void rp_view_pin_end(void){}
nvn_gpu_view rp_view_by_id(int i,nvn_gpu_cmd c,tex_t *t){(void)i;(void)c;if (t) memset(t, 0, sizeof *t);return NULL;}
uint32_t rp_storage_srgb_alias(uint32_t f) { (void)f; return 0; }
nvn_gpu_view rp_view_by_id_decl(int i,uint8_t t,uint8_t k,int e,nvn_gpu_cmd c,tex_t *o){(void)i;(void)t;(void)k;(void)e;(void)c;if (o) memset(o, 0, sizeof *o);return NULL;}
nvn_gpu_view rp_view_by_ubo_handle_decl(int i,uint8_t t,uint8_t k,nvn_gpu_cmd c,tex_t *o){return rp_view_by_id_decl(i,t,k,0,c,o);}   // 夹具不另判「没登记」：同 rp_view_by_id_decl 替身
void rp_decl_probe_begin(void){} int rp_decl_probe_end(void){return 0;}
nvn_gpu_pool_table rp_pool_table(nvn_gpu_cmd c,int samp,int base,uint32_t n,uint8_t dtt,uint8_t dcls,const char **why){(void)c;(void)samp;(void)base;(void)n;(void)dtt;(void)dcls;if(why)*why="pool_table_unresolved";return NULL;}   // 本夹具不出整池数组
nvn_gpu_samp rp_samp_by_id(int i){(void)i;return NULL;}
nvn_gpu_samp rp_samp_for_tex(int sid, int tid) { (void)tid; return rp_samp_by_id(sid); }   // #1072：替身不判过滤能力
nvn_gpu_samp rp_samp_for_const_handle(int sid, int tid, int pb, int pn) { (void)pb; (void)pn; return rp_samp_for_tex(sid, tid); }   // 同 rp_samp_for_tex：替身不给默认采样器
// 夹具不提供查询采样器；请求该能力时返回不可用。
nvn_gpu_samp rp_sampler_for_query(void) { return NULL; }
uint32_t rp_fmt_caps(uint32_t f) { return nvn_gpu_format_caps(f); }
int rp_tex_snap_by_id(int i,tex_t *t,view_t *v,int *h){(void)i;(void)t;(void)v;(void)h;return 0;}
uint64_t rp_tex_reg_gen(void){return 1;}uint64_t rp_tex_fact_sig(const uint64_t *h,int n,int b,int c){(void)h;(void)n;(void)b;(void)c;return 0;}
void rp_tex_mark_gpu_written(const tex_t *t){(void)t;}int rp_tex_debug_state(uint64_t s){(void)s;return 0;}
int nvn_be_tex_gpu_owned(uint64_t s) { return rp_tex_backend_by_serial(s) != NULL; }
nvn_gpu_tex rp_tex_backend_by_serial(uint64_t s){for(int i=0;i<3;i++)if(textures[i].serial==s)return images[i];return NULL;}
int rp_tex_is_read(const tex_t *t,const rp_pass_read *reads,int n){(void)t;(void)reads;(void)n;REQUIRE(0);return 0;} // 此夹具没有采样或图像绑定。
int rp_tex_storage_aliases(const tex_t *a,const tex_t *b){(void)a;(void)b;return 0;}
nvn_gpu_tex rp_tex_resolve(const tex_t *t,nvn_gpu_cmd c){(void)c;REQUIRE(t);nvn_gpu_tex g=rp_tex_backend_by_serial(t->serial);REQUIRE(g);return g;}
nvn_gpu_view rp_view_for(const tex_t *t,const view_t *v,nvn_gpu_cmd c){(void)c;REQUIRE(t && !v);for(int i=0;i<3;i++)if(textures[i].serial==t->serial)return views[i];REQUIRE(0);return NULL;}
nvn_gpu_view rp_view_for_attach(const tex_t *t,const view_t *v,nvn_gpu_cmd c){return rp_view_for(t,v,c);}
int rp_tex_take_zero(const tex_t *t){(void)t;return 0;}
int nvn_dirty_log_open(const void *s, uint64_t *i, uint64_t *c) { (void)s; (void)i; (void)c; return 0; }   // 本夹具没有脏页追加日志：回放核心走原路径
int nvn_dirty_log_read(const void *s, uint64_t i, uint64_t *c, void (*f)(void *, uintptr_t, uintptr_t), void *u) { (void)s; (void)i; (void)c; (void)f; (void)u; return 0; }
int nvn_dirty_page_armed(const void *s, const void *a) { (void)s; (void)a; return 0; }
int nvn_dirty_block_snapshot(const void *s, const void *a, size_t n, uint64_t *v, uint64_t *e) {
    (void)s; (void)a; (void)n; (void)v; (void)e; return 0;
}

static void clear_color(int slot,int mask,float r,float g,float b) {
    p_clear_color cc={0};cc.index=slot;cc.mask=mask;cc.rgba[0]=r;cc.rgba[1]=g;cc.rgba[2]=b;cc.rgba[3]=1;
    rp_pass_clear_color(active,&cc,&cc);
}
static void draw_mask(uint32_t mask) {
    rp_S.chmask.mask=mask;rp_pso_dirty=1;
    p_draw dr={0};dr.prim=4;dr.count=3;dr.instances=1;rp_draw(active,&dr,NULL,&dr);
}
static unsigned attached(unsigned p) {
    REQUIRE(p<npass);unsigned mask=0;for(unsigned i=0;i<8;i++)if(passes[p].color[i].view)mask|=1u<<i;return mask;
}
static void run_case(struct s2a_vk_device *device,const char *dir,int which) {
    enum {W=64,H=36,BYTES=W*H*4};
    nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="replay-zero-mask";GPU(nvn_gpu_open(&od,&err));
    const int side_effect=which>=5,depth=which==6;
    nvn_gpu_caps caps;nvn_gpu_caps_get(&caps);
    REQUIRE(!(caps.caps & NVN_GPU_CAP_NO_ATTACH_PASS)); // production capability, never overridden by this fixture
    if(side_effect)REQUIRE(caps.atomic_stages & (1u<<NVN_GPU_SH_FS));
    memset(&prog,0,sizeof prog);prog.color_out_mask=0xff;
    nvn_gpu_layout layout={0};
    if(side_effect){
        nvn_gpu_layout_ent *e=&prog.ent[1][0];prog.nent[1]=1;e->cls=NVN_GPU_RES_SSBO;e->src_kind=NVN_RB_SBUFP;
        e->count=1;e->access=NVN_GPU_ACC_RW;e->stages=1u<<NVN_GPU_SH_FS;layout.ent=e;layout.nent=1;
    }
    prog.sh[0]=shader(dir,"depth_state.vert",NVN_GPU_SH_VS,NULL);
    prog.sh[1]=shader(dir,side_effect?"zero_mask_side_effect.frag":"zero_mask_mrt.frag",NVN_GPU_SH_FS,side_effect?&layout:NULL);
    memset(textures,0,sizeof textures);memset(images,0,sizeof images);memset(views,0,sizeof views);
    for(int i=0;i<2+depth;i++){
        nvn_gpu_tex_desc td={0};td.d.target=NVN_TEXTURE_TARGET_2D;td.d.format=i==2?NVN_FORMAT_DEPTH24_STENCIL8:NVN_FORMAT_RGBA8;
        td.d.width=W;td.d.height=H;td.d.depth=td.d.levels=1;td.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
        GPU(nvn_gpu_tex_create(&td,&images[i],&err));GPU(nvn_gpu_view_create(images[i],NULL,&views[i],&err));
        textures[i].d=td.d;textures[i].serial=(uint64_t)i+1;
    }
    uint8_t sentinel[BYTES*2+32];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf readback=buffer(sizeof sentinel,sentinel);
    rp_pass_reset();rp_draw_reset();rp_state_reset();nvn_unimpl_reset();memset(guest,0,sizeof guest);write_id=0;
    rp_S.color.logic_op=3;rp_S.poly.mode=NVN_POLYGON_MODE_FILL;rp_S.chmask.mask=0xff;
    completed=0;GPU(nvn_gpu_cmd_begin(NULL,&active,&err));serial++;npass=0;
    p_set_rt target={0};target.n=2;target.color[0]=&textures[0];target.color[1]=&textures[1];if(depth)target.depth=&textures[2];
    rp_pass_set_rt(active,&target);clear_color(0,15,0,0,1);clear_color(1,15,0,0,1);
    if(depth){p_clear_ds ds={0};ds.depth=1;ds.depth_mask=1;ds.stencil_mask=0xff;rp_pass_clear_ds(active,&ds,&ds);}
    rp_pass_flush(active);npass=0;
    nvn_gpu_buf mirror=NULL;uint64_t mirror_off=0;
    if(side_effect){
        rp_S.ssbo[NVN_GPU_SH_FS][0].addr=(uint64_t)(uintptr_t)guest;rp_S.ssbo[NVN_GPU_SH_FS][0].size=16;
        REQUIRE(rp_buf_resolve(active,(uint64_t)(uintptr_t)guest,16,&mirror,&mirror_off));
        // The logical attachments, not this larger viewport, bound rasterization.
        rp_S.has_vp=1;rp_S.vp[0][2]=80;rp_S.vp[0][3]=50;rp_S.vp[0][5]=1;
    }
    unsigned before=emitted,expected_draws=1,expected_passes=1;
    uint8_t expected[2][4]={{255,0,0,255},{0,0,255,255}};
    switch(which){
    case 0: draw_mask(0xf);break;
    case 1:
        draw_mask(0xf);draw_mask(0x7);draw_mask(0xf0);draw_mask(0xf);expected_draws=4;expected_passes=3;
        expected[1][1]=255;expected[1][2]=0;break;
    case 2:
        clear_color(1,15,0,1,0);draw_mask(0xf);expected_passes=2;expected[1][1]=255;expected[1][2]=0;break;
    case 3:
        draw_mask(0xf);clear_color(1,2,0,1,0);draw_mask(0xf);expected_draws=2;expected_passes=3;expected[1][1]=255;break;
    case 4:
        target.color[1]=&textures[0];rp_pass_set_rt(active,&target);draw_mask(0xf0);expected[0][0]=0;expected[0][1]=255;break;
    default:
        draw_mask(0);expected[0][0]=0;expected[0][2]=255;break;
    }
    rp_pass_flush(active);
    REQUIRE(!nvn_unimpl_kinds());REQUIRE(emitted-before==expected_draws);REQUIRE(npass==expected_passes);
    if(which==0||which==2)REQUIRE(attached(0)==1);
    if(which==1){REQUIRE(attached(0)==1);REQUIRE(attached(1)==2);REQUIRE(attached(2)==1);}
    if(which==2)REQUIRE((attached(1)&2) && passes[1].color[1].load==NVN_GPU_LOAD_CLEAR);
    if(which==3){REQUIRE(attached(0)==1);REQUIRE(attached(1)==2);REQUIRE(attached(2)==1);}
    if(which==4)REQUIRE(attached(0)==2);
    if(which==5)REQUIRE(attached(0)==3); // no zero-attachment capability: preserve existing real draw
    if(which==6)REQUIRE(attached(0)==0 && passes[0].depth.view && passes[0].stencil.view);
    for(int i=0;i<2;i++)nvn_gpu_copy_tex_buf(active,images[i],0,0,NULL,readback,(uint64_t)i*BYTES,W*4,BYTES);
    GPU(nvn_gpu_cmd_commit(active,1,done,NULL,&err));GPU(nvn_gpu_fence_wait(1,5000000000ull,&err));completed=1;
    const uint8_t *bytes=nvn_gpu_buf_cpu(readback);unsigned bad=0;
    for(unsigned i=0;i<2;i++)for(unsigned p=0;p<W*H;p++)if(memcmp(bytes+i*BYTES+p*4,expected[i],4))bad++;
    for(unsigned i=BYTES*2;i<sizeof sentinel;i++)if(bytes[i]!=0xa5)bad++;
    uint32_t fragments=0;
    if(side_effect){memcpy(&fragments,(const uint8_t*)nvn_gpu_buf_cpu(mirror)+mirror_off,4);REQUIRE(fragments==W*H);rp_buf_gpu_write_land(write_key,write_id,0);write_id=0;}
    printf("zero-mask dq=%d case=%d draws=%u passes=%u pixels=%u bad=%u fragments=%u\n",dq_mode,which,emitted-before,npass,W*H*2,bad,fragments);
    cases++;failures+=bad;
    rp_pass_reset();rp_draw_reset();rp_buf_reset();nvn_gpu_shader_release(prog.sh[0]);nvn_gpu_shader_release(prog.sh[1]);
    for(int i=0;i<2+depth;i++){nvn_gpu_view_release(views[i]);nvn_gpu_tex_release(images[i]);}
    nvn_gpu_buf_release(readback,NULL,NULL);GPU(nvn_gpu_close(5000000000ull,&err));
}

enum pdc_case { VTX_GAP, VTX_HIT, UBO_GAP, UBO_HIT, LIST_BOUNDARY, DISABLED,
                INDEX_U16, INDEX_U32, INDEX_HIT, TOPO_STRIP, TOPO_FAN,
                TOPO_POINTS, TOPO_LINES, TOPO_LINE_STRIP, TOPO_BOUNDARY, TOPO_INDEX_HIT, PDC_CASES };

static void run_pdc_case(struct s2a_vk_device *device, const char *dir, enum pdc_case which, uint8_t *reference,
                         int save_reference) {
    enum { W = 32, H = 32, BYTES = W * H * 4 };
    const uint32_t topology[PDC_CASES] = {
        [TOPO_STRIP] = NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, [TOPO_FAN] = NVN_DRAW_PRIMITIVE_TRIANGLE_FAN,
        [TOPO_POINTS] = NVN_DRAW_PRIMITIVE_POINTS, [TOPO_LINES] = NVN_DRAW_PRIMITIVE_LINES,
        [TOPO_LINE_STRIP] = NVN_DRAW_PRIMITIVE_LINE_STRIP, [TOPO_BOUNDARY] = NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP,
        [TOPO_INDEX_HIT] = NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP,
    };
    const uint32_t prim = which >= TOPO_STRIP ? topology[which] : NVN_DRAW_PRIMITIVE_TRIANGLES;
    const int uniform = which == UBO_GAP || which == UBO_HIT;
    const int overlap = which == VTX_HIT || which == UBO_HIT;
    const int want_discard = !save_reference && (which == VTX_GAP || which == UBO_GAP ||
        which == INDEX_U16 || which == INDEX_U32 || (which >= TOPO_STRIP && which < TOPO_BOUNDARY));
    const int indexed = (which >= INDEX_U16 && which <= TOPO_STRIP) ||
                        which == TOPO_LINES || which == TOPO_LINE_STRIP || which == TOPO_BOUNDARY || which == TOPO_INDEX_HIT;
    pdc_enabled = 1;
    pdc_scope = which != DISABLED && !save_reference;
    rp_pass_reset(); rp_draw_reset(); rp_state_reset(); nvn_unimpl_reset();
    memset(guest, 0, sizeof guest);
    const nvn_gpu_open_desc od = {.host_device = device, .app_label = "replay-pass-discard-scope"};
    GPU(nvn_gpu_open(&od, &err));
    memset(&prog, 0, sizeof prog);
    prog.color_out_mask = 0xff;
    const nvn_gpu_layout_ent entry = {.set = 0, .binding = 0, .cls = NVN_GPU_RES_UBO,
        .src_kind = NVN_RB_CBUF, .count = 1, .access = NVN_GPU_ACC_R, .stages = 1u << NVN_GPU_SH_FS};
    const nvn_gpu_layout layout = {.ent = &entry, .nent = 1};
    if (uniform) {
        prog.nent[1] = 1;
        prog.ent[1][0] = entry;
        prog.sh[0] = shader(dir, "depth_state.vert", NVN_GPU_SH_VS, NULL);
        prog.sh[1] = shader(dir, "pdc_scope_ubo.frag", NVN_GPU_SH_FS, &layout);
        const float value[4] = {0, 1, 0, 1};
        uint8_t *const data = guest + (overlap ? 0 : 2 * BYTES);
        memcpy(data, value, sizeof value);
        rp_S.ubo[1][0] = (p_bind_buf){.addr = (uintptr_t)data, .size = sizeof value};
    } else {
        prog.attr_mask = 1;
        prog.sh[0] = shader(dir, which == TOPO_POINTS ? "pdc_point.vert" : "tri3.vert", NVN_GPU_SH_VS, NULL);
        prog.sh[1] = shader(dir, "zero_mask_mrt.frag", NVN_GPU_SH_FS, NULL);
        const float verts[3][4] = {{-1,-1,0,1}, {3,-1,0,1}, {-1,3,0,1}};
        const float sparse[3][4] = {{-0.6f,-0.5f,0,1}, {0.6f,-0.5f,0,1}, {0.6f,0.5f,0,1}};
        uint8_t *const data = guest + (overlap ? 0 : 2 * BYTES);
        memcpy(data, reference ? sparse : verts, sizeof verts);
        rp_S.nattr = rp_S.nstream = 1;
        rp_S.attr[0] = (va_st){.fmt = NVN_FORMAT_RGBA32F};
        rp_S.stream[0].stride = 16;
        rp_S.vbuf[0].address = (uintptr_t)data;
        rp_S.vbuf[0].size = sizeof verts;
    }
    rp_S.color.logic_op = NVN_LOGIC_OP_COPY;
    rp_S.chmask.mask = 0xff;
    pool_t pool = {.mem = guest, .size = sizeof guest, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    p_set_rt target = {.n = 2};
    for (int i = 0; i < 2; i++) {
        const nvn_gpu_tex_desc td = {.d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
            .width = W, .height = H, .depth = 1, .levels = 1}, .usage = NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
        GPU(nvn_gpu_tex_create(&td, &images[i], &err));
        GPU(nvn_gpu_view_create(images[i], NULL, &views[i], &err));
        textures[i] = (tex_t){.serial = (uint64_t)i + 1, .mem = guest + i * BYTES, .off = i * BYTES,
            .pool = &pool, .d = td.d, .L = {.size = BYTES}};
        target.color[i] = &textures[i];
    }
    completed = 0; serial++; npass = 0;
    GPU(nvn_gpu_cmd_begin(NULL, &active, &err));
    rp_pass_set_rt(active, &target);
    clear_color(0, 15, 0, 0, 1); clear_color(1, 15, 0, 0, 1);
    rp_pass_flush(active);
    npass = 0;
    struct {
        op_hdr draw_header;
        p_draw draw;
        op_hdr clear_header;
        p_clear_color clear;
        op_hdr end;
    } list = {.draw_header = {OP_DRAW, sizeof(op_hdr) + sizeof(p_draw)},
        .draw = {.prim = prim, .count = 3, .instances = 1},
        .clear_header = {OP_CLEAR_COLOR, sizeof(op_hdr) + sizeof(p_clear_color)},
        .clear = {.mask = 15, .rgba = {0,0,1,1}}, .end = {OP_END, sizeof(op_hdr)}};
    if (which == LIST_BOUNDARY || which == TOPO_BOUNDARY) list.clear_header = list.end;
    struct {
        op_hdr draw_header;
        p_draw_idx draw;
        op_hdr clear_header;
        p_clear_color clear;
    } index_list = {.draw_header = {OP_DRAW_IDX, sizeof(op_hdr) + sizeof(p_draw_idx)},
        .draw = {.prim = prim, .count = 3, .instances = 1},
        .clear_header = list.clear_header, .clear = list.clear};
    if (indexed) {
        const uint16_t u16[] = {0, 1, 2};
        const uint32_t u32[] = {0, 1, 2};
        uint8_t *const indices = guest + (which == INDEX_HIT || which == TOPO_INDEX_HIT ? 16 : 2 * BYTES + 64);
        index_list.draw.ibuf = (uintptr_t)indices;
        index_list.draw.itype = which == INDEX_U32 ? 2 : 1;
        if (which == INDEX_U32) memcpy(indices, u32, sizeof u32);
        else memcpy(indices, u16, sizeof u16);
    }
    const unsigned before = emitted;
    pdc_loads = pdc_dont_care = 0;
    watch_pdc = 1;
    rp_draw_stream = indexed ? &index_list.draw_header : &list.draw_header;
    rp_draw(active, indexed ? NULL : &list.draw, indexed ? &index_list.draw : NULL, rp_draw_stream);
    rp_draw_stream = NULL;
    watch_pdc = 0;
    REQUIRE(emitted == before + 1 && npass == 1);
    rp_pass_clear_color(active, &list.clear, &list.clear_header);
    rp_pass_flush(active);
    REQUIRE(npass == 2 && passes[1].color[0].load == NVN_GPU_LOAD_CLEAR);
    REQUIRE(!nvn_unimpl_kinds());
    nvn_gpu_buf readback = buffer(2 * BYTES, NULL);
    for (int i = 0; i < 2; i++)
        nvn_gpu_copy_tex_buf(active, images[i], 0, 0, NULL, readback, i * BYTES, W * 4, BYTES);
    GPU(nvn_gpu_cmd_commit(active, 1, done, NULL, &err));
    GPU(nvn_gpu_fence_wait(1, 5000000000ull, &err));
    completed = 1;
    const uint8_t *const pixels = nvn_gpu_buf_cpu(readback);
    REQUIRE(pixels);
    const uint8_t expect[2][4] = {{0,0,255,255}, {0,255,0,255}};
    unsigned bad = 0, written = 0;
    for (unsigned p = 0; p < W * H; p++) {
        bad += memcmp(pixels + p * 4, expect[0], 4) != 0;
        const uint8_t *const kept = pixels + BYTES + p * 4;
        const int drawn = !memcmp(kept, expect[1], 4);
        written += drawn;
        if (reference && save_reference)
            bad += !drawn && memcmp(kept, expect[0], 4) != 0;
        else
            bad += memcmp(kept, reference ? reference + p * 4 : expect[1], 4) != 0;
    }
    if (reference) {
        REQUIRE(written > 0 && written < W * H);
        if (save_reference) memcpy(reference, pixels + BYTES, BYTES);
    }
    printf("pdc scope dq=%d case=%d prim=0x%x reference=%d store_dont_care=%u expected=%d pixels=%d bad=%u written=%u\n",
           dq_mode, which, prim, save_reference, pdc_dont_care, want_discard, 2 * W * H, bad, written);
    pdc_cases++;
    pdc_failures += bad || pdc_loads != 2 || pdc_dont_care != (unsigned)want_discard ||
        passes[0].terminal_discard != want_discard;
    rp_pass_reset(); rp_draw_reset(); rp_buf_reset();
    nvn_gpu_shader_release(prog.sh[0]); nvn_gpu_shader_release(prog.sh[1]);
    for (int i = 0; i < 2; i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(images[i]); }
    nvn_gpu_buf_release(readback, NULL, NULL);
    GPU(nvn_gpu_close(5000000000ull, &err));
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    dq_mode=atoi(argv[2]);REQUIRE(dq_mode==0||dq_mode==1);
    struct s2a_vk_device device;char why[256];
    if(s2a_vk_device_create(&device,why,sizeof why)){fprintf(stderr,"%s\n",why);return 2;}
    for(int c=0;c<7;c++)run_case(&device,argv[1],c);
    for (enum pdc_case c = VTX_GAP; c < PDC_CASES; c++) {
        uint8_t reference[32 * 32 * 4];
        const int sparse = c >= TOPO_POINTS && c <= TOPO_LINE_STRIP;
        if (sparse) run_pdc_case(&device, argv[1], c, reference, 1);
        run_pdc_case(&device, argv[1], c, sparse ? reference : NULL, 0);
    }
    uint64_t records=nvn_vk_dbg_dq_recs();REQUIRE(dq_mode?records>0:records==0);
    REQUIRE(!s2a_vk_device_destroy(&device,1));
    printf("zero-mask cases=%u pixel-failures=%u dq-records=%llu\n",cases,failures,(unsigned long long)records);
    printf("pdc scope cases=%u failures=%u\n", pdc_cases, pdc_failures);
    return failures || pdc_failures ? 1 : 0;
}
