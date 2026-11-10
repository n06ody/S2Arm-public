// Exercise production session teardown with deterministic device and retirement outcomes.
#include "../native/nvn/nvn_vk_session.c"
int nvn_vk_session_destroy_with_callback(struct nvn_vk_session *s, void (*release)(struct s2a_vk_device *));
#include <assert.h>
#include <string.h>
static int idle_rc, settle_failure, swap_rc, calls, events[32], n;
static struct s2a_vk_device *expected_device;
enum { RECORD=1,CLEAR,TEX,IDLE,SWAP,FIRST,PIPE,TEX_DESTROY,SAMPLER,DEVICE };
static void event(int x) { events[n++]=x; }
int s2a_record_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l) {(void)d;(void)t; event(RECORD); *l=settle_failure && ++calls==2 ? 1:0; return 0;}
int s2a_vk_clear_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l) {(void)d;(void)t;event(CLEAR);*l=0;return 0;}
int nvn_vk_texcache_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l) {(void)d;(void)t;event(TEX);*l=0;return 0;}
int s2a_vk_device_wait_idle(struct s2a_vk_device*d){(void)d;event(IDLE);return idle_rc;}
int nvn_vk_swapchain_destroy(struct nvn_vk_swapchain*s){(void)s;event(SWAP);return swap_rc;}
void nvn_vk_first_draw_release_device(struct s2a_vk_device *d){assert(d==expected_device);event(FIRST);}
void nvn_vk_pipeline_destroy(struct nvn_vk_pipeline_cache*c){(void)c;event(PIPE);}
void nvn_vk_texcache_destroy(struct nvn_vk_texcache*c){(void)c;event(TEX_DESTROY);}
void nvn_vk_sampler_cache_destroy(struct nvn_vk_sampler_cache*c){(void)c;event(SAMPLER);}
int s2a_vk_device_destroy(struct s2a_vk_device*d,int proof){(void)d;assert(proof==1);event(DEVICE);return 0;}
// 读回 staging 缓存的收尾（真实实现见 nvn/nvn_vk_readback.c）：本用例把所有“设备子对象释放”
// 都换成确定性替身，这里同样不碰设备；顺序断言只关心 swap/first_draw/device 三者。
void nvn_vk_session_read_texels_release(struct nvn_vk_session *s){(void)s;}
// compute PSO 进程内缓存同点释放（nvn_vk_session.c:288，292b8a24 新增）：同口径确定性替身。
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *d){(void)d;}
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
static int pos(int e){for(int i=0;i<n;i++) if(events[i]==e) return i; return -1;}
static void test(int mode) {
 struct nvn_vk_session s={0}; s.ready=1; s.has_window=1;
 expected_device=&s.device; n=calls=0; idle_rc=mode==1 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS; settle_failure=mode==2; swap_rc=mode==3 ? -1 : 0;
 int rc=nvn_vk_session_destroy_with_callback(&s, nvn_vk_first_draw_release_device);
 if(mode) { assert(rc!=0); assert(s.ready); assert(pos(FIRST)<0 && pos(PIPE)<0 && pos(TEX_DESTROY)<0 && pos(SAMPLER)<0 && pos(DEVICE)<0); }
 else { assert(rc==0 && !s.ready); assert(pos(SWAP)>=0 && pos(FIRST)>pos(SWAP) && pos(DEVICE)>pos(FIRST)); }
}
int main(void){for(int i=1;i<4;i++) test(i); puts("failure retention passed"); fflush(stdout); test(0); puts("session release ordering passed"); return 0;}
