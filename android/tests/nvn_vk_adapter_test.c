#include <stdio.h>
#include "mtl_vk_adapter.h"
#include "nvn_obj.h"
#include <string.h>
// 本探针只验适配层的会话/纹理/池/记录接线，不覆盖真实绘制与上屏。适配层与会话引用的绘制入口
// 在这里给出显式桩：flush/present_done 没有录制过任何 draw，空操作就是正确语义；真正走到 draw
// 构建时返回 Unsupported 并写明原因，不伪造成功。（nvn_vk_draw.c 依赖编译请求执行器，未编入本探针。）
int nvk_draw_builder(struct s2a_vk_device *device, const struct s2a_record_draw_state *state,
                     struct s2a_record_result *result, void *opaque) {
    (void)device; (void)state; (void)opaque;
    if (result) {
        result->code = S2A_RECORD_UNSUPPORTED;
        snprintf(result->why, sizeof result->why, "adapter probe does not exercise the draw builder");
    }
    return S2A_RECORD_UNSUPPORTED;
}
int nvn_vk_first_draw_flush(void) { return 0; }
// F4.4/§6.1：adapter 的 guest fence 完成证明入口。探针只测 init/shutdown，这里给出“已完成”的
// fixture（不链接真实 first_draw，避免与其余 stub 冲突）；真实语义由 first_draw/游戏侧验收。
int nvn_vk_first_draw_flush_ready(int64_t budget_ns) { (void)budget_ns; return 0; }
void nvn_vk_first_draw_present_done(void) {}
int nvn_vk_first_draw_finish(void) { return 0; }
int nvn_vk_first_draw_retire_all(void) { return 0; }
int nvn_vk_query_terminal_ready(void) { return 1; }
void nvn_vk_first_draw_release_device(struct s2a_vk_device *d) { (void)d; }
int nvn_vk_first_draw_pass_break(int sync) { (void)sync; return 0; }
const char *nvn_vk_first_draw_first_error(void) { return ""; }
// #473 给 session 收尾加了 compute PSO 缓存释放；本探针不编 nvn_vk_compute.c，
// 空表语义下同桩——只提供链接符号（与上面 first_draw 各桩同口径）。
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *d) { (void)d; }

static uint8_t g_mem[256];
static int a_rd(void *o, uint64_t a, void *b, size_t n) { (void)o; if (a + n > sizeof g_mem) return -1; memcpy(b, g_mem + a, n); return 0; }
static int a_wr(void *o, uint64_t a, const void *b, size_t n) { (void)o; if (a + n > sizeof g_mem) return -1; memcpy(g_mem + a, b, n); return 0; }

int main(void) {
    char why[192] = "";
    if (s2a_vk_adapter_init(why, sizeof why)) { printf("FAIL init: %s\n", why); return 1; }
    int ok = s2a_vk_adapter_ready() && s2a_vk_adapter_session() != NULL;
    static char pool[4096];
    ok = ok && s2a_vk_adapter_pool_add(pool, sizeof pool) == 0;
    ok = ok && s2a_vk_adapter_pool_contains((uint64_t)(uintptr_t)pool + 16, 32);
    s2a_vk_adapter_pool_del(pool);
    ok = ok && !s2a_vk_adapter_pool_contains((uint64_t)(uintptr_t)pool + 16, 32);
    printf("pool: add/contains/del ok=%d\n", ok);
    uint8_t px[16] = {0,255,0,255, 0,255,0,255, 0,255,0,255, 0,255,0,255};
    tex_t tex; memset(&tex, 0, sizeof tex); tex.magic = 0x54584554; tex.d.target = 1; tex.d.format = 0x25;
    tex.d.width = 2; tex.d.height = 2; tex.d.stride = 8; tex.d.levels = 1; tex.mem = px; tex.serial = 1;
    char twhy[160] = "";
    struct nvn_vk_tex *vt = s2a_vk_adapter_tex(&tex, twhy, sizeof twhy);
    ok = ok && vt != NULL;
    printf("tex: %p %s\n", (void *)vt, twhy);
    {
        uint8_t pkt[40] = {0};
        struct s2a_record_memory m = { NULL, a_rd, a_wr };
        op_hdr hh = { OP_UPDATE_UB, sizeof pkt }; memcpy(pkt, &hh, sizeof hh);
        uint64_t dst = 32, bytes = 4; memcpy(pkt + 8, &dst, 8); memcpy(pkt + 16, &bytes, 8);
        uint32_t w = 0xA5A5F00D; memcpy(pkt + 24, &w, 4);
        struct s2a_record_result r;
        int rc = s2a_vk_adapter_exec(pkt, sizeof pkt, &m, &r);
        uint32_t got = 0; memcpy(&got, g_mem + 32, 4);
        ok = ok && rc == 0 && got == 0xA5A5F00D;
        printf("exec: rc=%d code=%d got=%08x\n", rc, (int)r.code, got);
        static uint8_t guest[256];
        uint8_t gp[40] = {0};
        op_hdr gh = { OP_UPDATE_UB, sizeof gp }; memcpy(gp, &gh, sizeof gh);
        uint64_t gdst = (uint64_t)(uintptr_t)(guest + 64); memcpy(gp + 8, &gdst, 8);
        uint64_t gb = 4; memcpy(gp + 16, &gb, 8);
        uint32_t gw = 0x1234ABCD; memcpy(gp + 24, &gw, 4);
        struct s2a_record_result gr;
        int grc = s2a_vk_adapter_exec_guest(gp, sizeof gp, &gr);
        uint32_t ggot = 0; memcpy(&ggot, guest + 64, 4);
        ok = ok && grc == 0 && ggot == 0x1234ABCD;
        printf("exec-guest: rc=%d code=%d got=%08x\n", grc, (int)gr.code, ggot);
    }
    printf("adapter: ready=%d session=%p\n", s2a_vk_adapter_ready(), (void *)s2a_vk_adapter_session());
    s2a_vk_adapter_shutdown();
    ok = ok && !s2a_vk_adapter_ready();
    printf("nvn-vk-adapter: %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
