#include "window.h"
#include "host_android.h"
#include <cassert>
#include <cstdio>
static int instances,presenters;
static s2a_wsi_lease *published;
extern "C" void ANativeWindow_acquire(ANativeWindow *w) { ++w->refs; }
extern "C" void ANativeWindow_release(ANativeWindow *w) { --w->refs; }
extern "C" int32_t ANativeWindow_getWidth(ANativeWindow *w) { return w->width; }
extern "C" int32_t ANativeWindow_getHeight(ANativeWindow *w) { return w->height; }
// 真实语义：缓冲尺寸改写后 getWidth/getHeight 报告缓冲尺寸。
extern "C" int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t) {
    w->width = width; w->height = height; return 0;
}
extern "C" void s2a_android_host_set_lease(s2a_wsi_lease *l) { published=l; }
extern "C" void s2a_android_host_unset_lease(s2a_wsi_lease *l) { if(published==l)published=nullptr; }
extern "C" int __android_log_print(int,const char*,const char*,...) { return 0; }
extern "C" void android_set_abort_message(const char*) {}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*) { ++instances; return VK_ERROR_INITIALIZATION_FAILED; }
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance,const VkAllocationCallbacks*) {}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance,uint32_t*,VkPhysicalDevice*) { return VK_ERROR_INITIALIZATION_FAILED; }
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice,VkPhysicalDeviceProperties*) {}
Presenter::Presenter(VkInstance i,s2a_wsi_lease *l):instance_(i),lease_(l) { ++presenters; }
Presenter::~Presenter()=default;
bool Presenter::stop(int){return true;}
void Presenter::set_release_stall(int){}
void Presenter::set_stop_stall(int){}
int main(){
 ANativeWindow surface{1,1280,720};
 {
  AndroidWindow window(true); assert(published); assert(!window.ready_for_game());
  window.set_foreground(true); assert(!window.ready_for_game());
  window.offer(&surface); assert(surface.refs==1); assert(!window.ready_for_game());
  const int32_t insets[4]={0,0,0,0};
  window.update(1280,720,1,insets,60); assert(window.ready_for_game());
  const auto *api=window.runtime_api(); assert(api && api->lease==published);
  s2a_wsi_state state{}; assert(s2a_wsi_take(published,&state)==0); assert(state.window==&surface && state.session==1 && state.width==1280 && state.revision==1);
  s2a_wsi_release_window(published,state.window); s2a_wsi_released(published,state.generation);
  window.set_foreground(false); assert(!window.ready_for_game());
  window.set_foreground(true); assert(window.ready_for_game());
  window.set_release_stall(1); window.set_stop_stall(1);
  window.release(); assert(surface.refs==0); assert(!window.ready_for_game());
  window.update(1280,720,1,insets,60); assert(!window.ready_for_game());
  assert(instances==0 && presenters==0);
 }
 assert(!published); assert(instances==0 && presenters==0);
 { AndroidWindow diagnostic; assert(instances==1); }
 puts("Window game mode: real lease offer/take/revoke + zero diagnostic Vulkan/presenter PASS (host boundary fakes; no GPU)");
}
