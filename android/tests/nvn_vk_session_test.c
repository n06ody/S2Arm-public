#include <stdio.h>
#include "nvn_vk_session.h"
#include <string.h>

// 本 probe 不创建 swapchain/present；swapchain 的两个 first_draw 入口不会被调用，
// 只提供链接符号。retirement 由 vk_record.c / vk_clear.c 的真实实现参与。
int nvn_vk_first_draw_flush(void) { return 0; }
void nvn_vk_first_draw_present_done(void) {}
// #473 给 session 收尾加了 compute PSO 缓存释放；本探针不编 nvn_vk_compute.c
// （会拖入 program 依赖链），空表语义下同桩——只提供链接符号。
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *d) { (void)d; }

int main(void) {
    struct nvn_vk_session s = {0}; char why[192] = "";
    if (nvn_vk_session_create(&s, why, sizeof why)) { printf("FAIL session: %s\n", why); return 1; }
    uint8_t px[16] = {0,255,0,255, 0,255,0,255, 0,255,0,255, 0,255,0,255};
    tex_t t; memset(&t, 0, sizeof t); t.magic = 0x54584554; t.d.target = 1; t.d.format = 0x25;
    t.d.width = 2; t.d.height = 2; t.d.stride = 8; t.d.levels = 1; t.mem = px; t.serial = 1;
    char twhy[160] = "";
    struct nvn_vk_tex *vt = nvn_vk_session_tex(&s, &t, twhy, sizeof twhy);
    printf("session: driver=%s ready=%d tex=%p %s\n", s.device.driver_name, s.ready, (void *)vt, twhy);
    nvn_vk_session_destroy(&s);
    printf("nvn-vk-session: %s\n", (s.ready || !vt) ? "FAIL" : "ok");
    return (s.ready || !vt) ? 1 : 0;
}
