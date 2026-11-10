// Real production preheat and session teardown; Linux lavapipe + validation required.
#ifndef REAL_FIRST_DRAW
#define REAL_FIRST_DRAW "../native/nvn/nvn_vk_first_draw.c"
#endif
#include REAL_FIRST_DRAW
#include "nvn_vk_session.h"
#include <assert.h>
int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
int __system_property_get(const char*n,char*v){(void)n;*v=0;return 0;}
// fd_lookup_clear/ensure 记 s2a_vk_adapter_registry_generation（mtl_vk_adapter.c 提供）：
// 本探针不碰真适配器注册表，代数以恒定非零值即可（运行期间注册表不变）。
uint64_t s2a_vk_adapter_registry_generation(void){return 1;}
int32_t ANativeWindow_getWidth(ANativeWindow*w){(void)w;return 0;}
int32_t ANativeWindow_getHeight(ANativeWindow*w){(void)w;return 0;}
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"failed: %s at %d\n",#x,__LINE__); return 2; } } while(0)
static uint8_t *read_file(const char *p,size_t *n){FILE*f=fopen(p,"rb");if(!f)return NULL;if(fseek(f,0,SEEK_END))return NULL;long z=ftell(f);if(z<=0)return NULL;rewind(f);uint8_t*b=malloc((size_t)z);if(!b)return NULL;if(fread(b,1,(size_t)z,f)!=(size_t)z)return NULL;fclose(f);*n=(size_t)z;return b;}
int main(int argc,char **argv){
 CHECK(argc==3); uint32_t nl=0;CHECK(vkEnumerateInstanceLayerProperties(&nl,NULL)==VK_SUCCESS&&nl);VkLayerProperties*ls=calloc(nl,sizeof *ls);CHECK(ls);CHECK(vkEnumerateInstanceLayerProperties(&nl,ls)==VK_SUCCESS);int found=0;for(uint32_t i=0;i<nl;i++)if(!strcmp(ls[i].layerName,"VK_LAYER_KHRONOS_validation"))found=1;free(ls);CHECK(found);CHECK(getenv("VK_INSTANCE_LAYERS")&&strstr(getenv("VK_INSTANCE_LAYERS"),"VK_LAYER_KHRONOS_validation"));
 struct nvn_vk_session session={0};char why[256]={0};CHECK(nvn_vk_session_create(&session,why,sizeof why)==0);struct s2a_vk_device*d=&session.device;CHECK(d->properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU && strstr(d->properties.deviceName,"llvmpipe"));fprintf(stderr,"device=%s layer=requested-and-available\n",d->properties.deviceName);
 CHECK(setenv("S2A_DIRECT_CACHE_DIR",argv[2],1)==0);size_t size=0;uint8_t*bytes=read_file(argv[1],&size);CHECK(bytes);uint8_t input[32]={7}, result_sha[32];s2a_sha256 hash;s2a_sha256_init(&hash);s2a_sha256_update(&hash,bytes,size);s2a_sha256_final(&hash,result_sha);CHECK(s2a_program_cache_store(argv[2],input,bytes,size,why,sizeof why)==0);free(bytes);
 CHECK(fd_pipeline_cache(d));struct nvk_pipe_recipe r={0};memcpy(r.prog_sha,result_sha,sizeof r.prog_sha);r.layout_policy=NVK_PIPE_LAYOUT_POLICY;r.rt_count=1;r.rt_color_count=1;r.rt_bound_mask=1;r.rt_fmt[0]=VK_FORMAT_R8G8B8A8_UNORM;r.prim=4;nvm_manifest_note(&r,input);CHECK(nvm_manifest_count()==1);fd_preheat_one(d);CHECK(atomic_load(&g_preheat_n)==1 && g_pipe_cache[0].used && g_pipe_cache[0].pipe && g_prog_obj[0].used);CHECK(g_pipeline_cache && g_driver_dev==d);
 CHECK(fd_arena_new_chunk(d,1024)==0 && g_arena_chunks[0].map);g_arena_parity=0;CHECK(fd_frame_pool(d,1));g_arena_parity=1;CHECK(fd_frame_pool(d,1));
 CHECK(nvn_vk_query_terminal_ready());CHECK(nvn_vk_session_destroy_with_callback(&session,nvn_vk_first_draw_release_device)==0);CHECK(!session.ready);
#ifndef TEARDOWN_NEGATIVE_CONTROL
 CHECK(atomic_load(&g_devmem_reg_n)==0);CHECK(atomic_load(&g_memcat[FD_MEM_ARENA].cur)==0);CHECK(atomic_load(&g_devmem_bytes)==0);
#endif
 fprintf(stderr,"production preheat and session teardown complete\n");return 0;
}
