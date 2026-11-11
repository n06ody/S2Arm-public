#!/usr/bin/env python3
"""Host-only original-function-body retirement contract test; no GPU/device claims.

Vulkan and ledger dependencies are controlled mocks; orchestration bodies come
from the actual source files. Expected state/counts follow owner pure-A contract.
--source-ref compiles the same tests against a git revision for red/withdrawal.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-ref", help="git revision for source withdrawal control")
args = parser.parse_args()

def source(path):
    if args.source_ref:
        return subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()

def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, i = 1, brace + 1
    while depth:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[start:i]

device = source("android/native/direct/vk_device.c")
session = source("android/native/nvn/nvn_vk_session.c")
new_api = "int s2a_vk_device_wait_idle(" in device
dev = function(device, ("int" if new_api else "void") + " s2a_vk_device_destroy(")
wait = function(device, "int s2a_vk_device_wait_idle(") if new_api else ""
destroy = function(session, "int nvn_vk_session_destroy(")
# Keep the real callback implementation before its public wrapper; old refs
# predate this split and still provide the complete body in destroy itself.
if "int nvn_vk_session_destroy_with_callback(" in session:
    destroy = function(session, "int nvn_vk_session_destroy_with_callback(") + "\n" + destroy
create = function(session, "int nvn_vk_session_create(")
# 本支 nvn_vk_session.c:284 在设备空闲证明成功之后、device_destroy 之前新增读回 staging
# 缓存释放（真实实现 nvn/nvn_vk_readback.c:233 → nvn_vk_readback_gpu_release）。旧 ref
# 没有这一步；用检测结果决定是否给夹具加这条 seam 的断言。
has_read_texels_release = "nvn_vk_session_read_texels_release(s);" in destroy
has_pso_cache_release = "nvn_vk_compute_pso_cache_release(&s->device);" in destroy + create
has_clear_fv_release = "s2a_vk_clear_fv_release_device(&s->device);" in destroy
new_session = function(source("android/native/nvn/mtl_vk_adapter.c"), "void s2a_vk_adapter_new_session(")
provider_source = source("android/native/direct/provider_vk.c")
# The metadata observer wraps, but does not alter, this retirement body. Its
# Stop/Destroy wrapper is exercised by w2_observation_lifecycle_test.py.
provider_signature = "static int32_t destroy_impl(" if "static int32_t destroy_impl(" in provider_source else "static int32_t destroy("
provider = function(provider_source, provider_signature).replace("destroy_impl(", "destroy(", 1)
preamble=r'''
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_INFO 4
#define VK_SUCCESS 0
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
struct s2a_vk_device { uintptr_t instance,device,command_pool; };
struct nvn_vk_session { struct s2a_vk_device device; int textures,samplers,pipelines,swapchain,ready,has_window; };
static int idle_rc,children_destroyed,device_destroyed,create_called,left=1,wait_called,abandoned,idle_proven,sticky,release_calls,swap_error,clear_left,tex_left,release_error;
static int vkDeviceWaitIdle(uintptr_t d){(void)d;wait_called++;if(idle_rc==0)idle_proven=1;return idle_rc;}
static void vkDestroyCommandPool(uintptr_t d,uintptr_t p,void *a){(void)d;(void)p;(void)a;children_destroyed++;}
static void vkDestroyDevice(uintptr_t d,void*a){(void)d;(void)a;device_destroyed++;}
static void vkDestroyInstance(uintptr_t d,void*a){(void)d;(void)a;}
static int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}
static int nvn_vk_swapchain_destroy(int*s){(void)s;if(swap_error)return -1;children_destroyed++;return 0;}
static void nvn_vk_pipeline_destroy(int*s){(void)s;children_destroyed++;}
static int texcache_destroyed;
static void nvn_vk_texcache_destroy(int*s){(void)s;children_destroyed++;texcache_destroyed++;}
static void nvn_vk_sampler_cache_destroy(int*s){(void)s;children_destroyed++;}
static int s2a_record_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l){(void)d;(void)t;release_calls++;if(idle_proven && !sticky && t>0)left=0;*l=left;return release_error;}
static int s2a_vk_clear_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l){(void)d;(void)t;*l=clear_left;return 0;}
static int nvn_vk_texcache_release_retained(struct s2a_vk_device*d,uint64_t t,uint32_t*l){(void)d;(void)t;*l=tex_left;return 0;}
static int s2a_vk_device_create(struct s2a_vk_device*d,char*w,size_t z){(void)w;(void)z;create_called++;d->device=7;return 0;}
static int nvn_vk_texcache_init(int*p,struct s2a_vk_device*d,char*w,size_t z){(void)p;(void)d;(void)w;(void)z;return 0;}
static void nvn_vk_sampler_cache_init(int*p,struct s2a_vk_device*d){(void)p;(void)d;}
static void nvn_vk_pipeline_init(int*p,struct s2a_vk_device*d){(void)p;(void)d;}
#define TAG "test"
#define S2A_DIRECT_RES_ARGUMENT -2
#define S2A_DIRECT_RES_RETIREMENT -9
#define S2A_DIRECT_RES_OK 0
struct provider_context { char shader_dir[1],window_error[1],reason[1]; int revoking,engine_ok,device_ok; struct s2a_vk_device device; };
static struct nvn_vk_session g_session;
static int g_mu,g_retire_failed; static _Atomic int g_stopped;   // 与 mtl_vk_adapter.c 同型：生产对它 atomic_store / atomic_load（Apple clang 拒绝对普通 int 做原子操作）
static struct provider_context *g_active_context,*g_retained_context;
static int g_window_mu,shutdown_calls,shutdown_error,free_calls; static _Atomic int g_stop_published,g_session_active;   // 与 provider_vk.c 同型（生产对它们 atomic_store）
static void diag_log_manifest(const char*a,const char*b){(void)a;(void)b;}
static void s2a_direct_unbind_compiler_v1(void*p){(void)p;}
static void ctl_wake(void){}
static void pthread_mutex_lock(int*p){(void)p;}
static void pthread_mutex_unlock(int*p){(void)p;}
static int s2a_vk_adapter_shutdown(void){shutdown_calls++;return shutdown_error;}
static void release_lease_window(void*p){(void)p;}
static void mock_free(void*p){(void)p;free_calls++;}

// 读回 staging 缓存的收尾（真实实现见 native/nvn/nvn_vk_readback.c:233；与
// nvn_vk_session_release_order_test.c 同口径：设备子对象全部用确定性替身，本用例不碰设备）。
// 生产顺序是“设备空闲证明成功之后、s2a_vk_device_destroy 之前”（nvn_vk_session.c:284），
// 这里顺带记录这两个前置条件，由成功用例断言；旧 ref 没有这次调用，所以断言按
// -DHAVE_READ_TEXELS_RELEASE 编译开关保留，--source-ref 的撤回对照不受影响。
static int read_texels_release_calls, read_texels_release_after_idle, read_texels_release_before_destroy;
void nvn_vk_session_read_texels_release(struct nvn_vk_session *s){(void)s;
 read_texels_release_calls++;
 read_texels_release_after_idle = wait_called == 1 && idle_proven;
 read_texels_release_before_destroy = device_destroyed == 0;}

// compute PSO 进程内缓存：对象属于本设备、必须先于 vkDestroyDevice 释放
//（nvn_vk_session.c:288，292b8a24 新增；create 失败回收路径 :22 同调）。与上面
// read_texels 同口径记调用时序，-DHAVE_PSO_CACHE_RELEASE 下断言，旧 ref 不受影响。
static int pso_release_calls, pso_release_before_destroy;
void nvn_vk_compute_pso_cache_release(struct s2a_vk_device *d){(void)d;
 pso_release_calls++;
 pso_release_before_destroy = device_destroyed == 0;}

#ifdef HAVE_CLEAR_FV_RELEASE
// 缓存引用纹理底图，必须在设备空闲后、纹理缓存和设备销毁前释放。
static int clear_fv_release_calls, clear_fv_release_ordered;
void s2a_vk_clear_fv_release_device(struct s2a_vk_device *d){(void)d;
 clear_fv_release_calls++;
 clear_fv_release_ordered = idle_proven && texcache_destroyed == 0 && device_destroyed == 0;}
#endif

'''
main=r'''
int main(int argc,char**argv){
 if(argc!=2)return 2;
 (void)g_mu;
 struct nvn_vk_session s={.device={.instance=1,.device=2,.command_pool=3},.ready=1};


 if(!strcmp(argv[1],"new-session-blocked")){
  g_stopped=1;g_retire_failed=1;g_session=s;s2a_vk_adapter_new_session();
  return !(g_stopped==1&&g_retire_failed==1&&g_session.device.device==2);
 }
 if(!strcmp(argv[1],"new-session-clean")){
  g_stopped=1;s2a_vk_adapter_new_session();return !(g_stopped==0&&g_retire_failed==0);
 }
 if(!strcmp(argv[1],"provider-retry")){
  struct provider_context ctx={.engine_ok=1,.device_ok=1,.device={.instance=1,.device=2,.command_pool=3}};
  g_active_context=&ctx;shutdown_error=-1;
  int first=destroy(&ctx),second=destroy(&ctx);
  return !(first==S2A_DIRECT_RES_RETIREMENT&&second==S2A_DIRECT_RES_RETIREMENT&&shutdown_calls==2&&ctx.engine_ok==1&&free_calls==0&&device_destroyed==0&&g_active_context==&ctx&&g_retained_context==&ctx);
 }
 if(!strcmp(argv[1],"device-error")){
  idle_rc=VK_ERROR_OUT_OF_HOST_MEMORY;DESTROY(&s.device);
  printf("device=%lu destroys=%d children=%d wait=%d\n",(unsigned long)s.device.device,device_destroyed,children_destroyed,wait_called);
  return !(s.device.device==2&&device_destroyed==0&&children_destroyed==0&&wait_called==1);
 }
 if(!strcmp(argv[1],"session-error")){
  idle_rc=VK_ERROR_OUT_OF_HOST_MEMORY;int r=nvn_vk_session_destroy(&s);
  printf("rc=%d ready=%d device=%lu destroys=%d children=%d left=%d\n",r,s.ready,(unsigned long)s.device.device,device_destroyed,children_destroyed,left);
  return !(r==-1&&s.ready==1&&s.device.device==2&&device_destroyed==0&&children_destroyed==0&&wait_called==1
#ifdef HAVE_CLEAR_FV_RELEASE
   &&clear_fv_release_calls==0
#endif
   );
 }
 if(!strcmp(argv[1],"session-success")){
  idle_rc=VK_SUCCESS;int r=nvn_vk_session_destroy(&s);
  printf("rc=%d ready=%d device=%lu destroys=%d left=%d\n",r,s.ready,(unsigned long)s.device.device,device_destroyed,left);
  return !(r==0&&s.ready==0&&s.device.device==0&&device_destroyed==1&&children_destroyed==4&&left==0&&wait_called==1&&release_calls==2
#ifdef HAVE_READ_TEXELS_RELEASE
   &&read_texels_release_calls==1&&read_texels_release_after_idle&&read_texels_release_before_destroy
#endif
#ifdef HAVE_PSO_CACHE_RELEASE
   &&pso_release_calls==1&&pso_release_before_destroy
#endif
#ifdef HAVE_CLEAR_FV_RELEASE
   &&clear_fv_release_calls==1&&clear_fv_release_ordered
#endif
   );
 }
 if(!strcmp(argv[1],"session-lost")){
  idle_rc=VK_ERROR_DEVICE_LOST;int r=nvn_vk_session_destroy(&s);
  printf("rc=%d ready=%d destroys=%d left=%d abandoned=%d\n",r,s.ready,device_destroyed,left,abandoned);
  return !(r==-1&&s.ready==1&&s.device.device==2&&device_destroyed==0&&children_destroyed==0&&left==1&&abandoned==0&&wait_called==1);
 }
 if(!strcmp(argv[1],"recreate")){
  char why[64];int r=nvn_vk_session_create(&s,why,sizeof why);
  printf("rc=%d device=%lu create_called=%d\n",r,(unsigned long)s.device.device,create_called);
  return !(r==-1&&s.device.device==2&&create_called==0);
 }

 if(!strcmp(argv[1],"clear-sticky")||!strcmp(argv[1],"upload-sticky")||!strcmp(argv[1],"settlement-error")){
  clear_left=!strcmp(argv[1],"clear-sticky");tex_left=!strcmp(argv[1],"upload-sticky");
  release_error=!strcmp(argv[1],"settlement-error")?-1:0;
  int r=nvn_vk_session_destroy(&s);
  return !(r==-1&&s.ready==1&&device_destroyed==0&&children_destroyed==0&&wait_called==1);
 }
 if(!strcmp(argv[1],"settlement-sticky")){
  sticky=1;int r=nvn_vk_session_destroy(&s);
  return !(r==-1&&s.ready==1&&s.device.device==2&&device_destroyed==0&&children_destroyed==0&&left==1&&wait_called==1);
 }
 if(!strcmp(argv[1],"empty-ledger-error")){
  left=0;idle_rc=VK_ERROR_OUT_OF_HOST_MEMORY;int r=nvn_vk_session_destroy(&s);
  return !(r==-1&&device_destroyed==0&&children_destroyed==0&&wait_called==1);
 }
 if(!strcmp(argv[1],"presentation-error")){
  s.has_window=1;swap_error=1;int r=nvn_vk_session_destroy(&s);
  return !(r==-1&&s.has_window==1&&s.ready==1&&device_destroyed==0&&children_destroyed==0);
 }
 if(!strcmp(argv[1],"empty-ledger-success")){
  left=0;int r=nvn_vk_session_destroy(&s);
  return !(r==0&&device_destroyed==1&&children_destroyed==4&&wait_called==1
#ifdef HAVE_READ_TEXELS_RELEASE
   &&read_texels_release_calls==1&&read_texels_release_before_destroy
#endif
#ifdef HAVE_PSO_CACHE_RELEASE
   &&pso_release_calls==1&&pso_release_before_destroy
#endif
#ifdef HAVE_CLEAR_FV_RELEASE
   &&clear_fv_release_calls==1&&clear_fv_release_ordered
#endif
   );
 }
 if(!strcmp(argv[1],"fresh-create")){
  struct nvn_vk_session fresh={0};char why[64];int r=nvn_vk_session_create(&fresh,why,sizeof why);
  return !(r==0&&fresh.ready==1&&fresh.device.device==7&&create_called==1);
 }
 return 2;
}
'''

cases = ["device-error", "session-error", "session-success", "session-lost", "recreate",
         "settlement-sticky", "empty-ledger-error", "presentation-error", "empty-ledger-success", "fresh-create", "provider-retry", "new-session-blocked", "new-session-clean", "clear-sticky", "upload-sticky", "settlement-error"]
import sys as _wsys
_wsys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

with tempfile.TemporaryDirectory(prefix="s2a-retirement-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    call = "#define DESTROY(d) s2a_vk_device_destroy(d, 0)\n" if new_api else "#define DESTROY(d) s2a_vk_device_destroy(d)\n"
    c.write_text(preamble + call + wait + "\n" + dev + "\n" + destroy + "\n" + create + "\n#define free mock_free\n" + provider + "\n#undef free\n" + new_session + "\n" + main)
    # -DHAVE_READ_TEXELS_RELEASE / -DHAVE_PSO_CACHE_RELEASE：新收尾调用的顺序断言开关；
    # macOS 弱引用放行由 host_link 按目标文件实算
    define = (["-DHAVE_READ_TEXELS_RELEASE"] if has_read_texels_release else []) + \
             (["-DHAVE_PSO_CACHE_RELEASE"] if has_pso_cache_release else []) + \
             (["-DHAVE_CLEAR_FV_RELEASE"] if has_clear_fv_release else [])
    host_link.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", *define, str(c)] + ["-o", str(exe)], check=True)
    failed = 0
    for case in cases:
        result = subprocess.run([str(exe), case], text=True, capture_output=True)
        print(f"{case}: exit={result.returncode} {result.stdout.strip()}")
        failed += result.returncode != 0
    print(f"{len(cases)-failed}/{len(cases)} passed (host mock, not Vulkan runtime)")
    raise SystemExit(bool(failed))
