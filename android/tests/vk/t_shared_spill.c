// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Execute the same shader before/after the production SPIR-V transformation.
// Reference fits the physical device; candidate is transformed against a
// deliberately smaller 4 KiB limit. Direct and indirect dispatch both run.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VK_OK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr,"Vulkan line %d: %s = %d\n",__LINE__,#x,r_); exit(2); } } while (0)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"assertion line %d: %s\n",__LINE__,#x); exit(2); } } while (0)
enum { NX = 2, NY = 3, NZ = 2, GROUPS = NX * NY * NZ, WORDS = GROUPS * 9 };
static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        __atomic_fetch_add(&validation_errors, 1, __ATOMIC_RELAXED);
        fprintf(stderr, "validation: %s\n", d && d->pMessage ? d->pMessage : "unknown");
    }
    return VK_FALSE;
}
struct buffer { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; };
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipeline_layout;
static VkDescriptorPool desc_pool;
static VkDescriptorSet desc_set;

static struct buffer buffer_make(VkDeviceSize size, VkBufferUsageFlags usage) {
    struct buffer b = {.size = size};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_OK(vkCreateBuffer(device, &bi, NULL, &b.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, b.buf, &req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { index = i; break; }
    CHECK(index != UINT32_MAX);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = index};
    VK_OK(vkAllocateMemory(device, &ai, NULL, &b.mem)); VK_OK(vkBindBufferMemory(device, b.buf, b.mem, 0));
    VK_OK(vkMapMemory(device, b.mem, 0, size, 0, &b.map));
    return b;
}
static void buffer_free(struct buffer *b) {
    vkUnmapMemory(device, b->mem); vkDestroyBuffer(device, b->buf, NULL); vkFreeMemory(device, b->mem, NULL);
}
static VkShaderModule shader_load(const char *path) {
    FILE *f = fopen(path, "rb"); CHECK(f); CHECK(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f); CHECK(n > 0 && !(n & 3)); rewind(f);
    uint32_t *p = malloc((size_t)n); CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n); fclose(f);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = p};
    VkShaderModule shader; VK_OK(vkCreateShaderModule(device, &ci, NULL, &shader)); free(p); return shader;
}
static void execute(const char *path, int indirect, struct buffer *out, struct buffer *spill, struct buffer *args, uint32_t result[WORDS]) {
    VkShaderModule shader = shader_load(path);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
        .layout = pipeline_layout};
    VkPipeline pipeline; VK_OK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    memset(out->map, 0xa5, (size_t)out->size); memset(spill->map, 0xcc, (size_t)spill->size);
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd; VK_OK(vkAllocateCommandBuffers(device, &ca, &cmd));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(cmd, &begin));
    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &before, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &desc_set, 0, NULL);
    if (indirect) vkCmdDispatchIndirect(cmd, args->buf, 0); else vkCmdDispatch(cmd, NX, NY, NZ);
    VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    VK_OK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    VK_OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); VK_OK(vkQueueWaitIdle(queue));
    memcpy(result, out->map, sizeof(uint32_t) * WORDS);
    vkFreeCommandBuffers(device, pool, 1, &cmd); vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, shader, NULL);
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: reference.spv candidate.spv\n"); return 2; }
    const char *layer = "VK_LAYER_KHRONOS_validation", *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext};
    VK_OK(vkCreateInstance(&ii, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create_debug = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    CHECK(create_debug && destroy_debug);
    VkDebugUtilsMessengerCreateInfoEXT di = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = callback};
    VkDebugUtilsMessengerEXT debug; VK_OK(create_debug(instance, &di, NULL, &debug));
    uint32_t n = 0; VK_OK(vkEnumeratePhysicalDevices(instance, &n, NULL)); CHECK(n > 0);
    VkPhysicalDevice *devices = calloc(n, sizeof *devices); CHECK(devices); VK_OK(vkEnumeratePhysicalDevices(instance, &n, devices)); physical = devices[0]; free(devices);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    CHECK(props.limits.maxComputeSharedMemorySize >= (1536 + 1024 + 1024) * 4);
    printf("physical=%s shared_limit=%u simulated_limit=4096\n", props.deviceName, props.limits.maxComputeSharedMemorySize);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, NULL); VkQueueFamilyProperties *qs = calloc(nq, sizeof *qs); CHECK(qs);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, qs); uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    free(qs); CHECK(family != UINT32_MAX);
    float priority = 1.0f; VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dev = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    VK_OK(vkCreateDevice(physical, &dev, NULL, &device)); vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family}; VK_OK(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo li = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VK_OK(vkCreateDescriptorSetLayout(device,&li,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};
    VK_OK(vkCreatePipelineLayout(device,&pl,NULL,&pipeline_layout));
    VkDescriptorPoolSize ps = {.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=2};
    VkDescriptorPoolCreateInfo dpi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VK_OK(vkCreateDescriptorPool(device,&dpi,NULL,&desc_pool));
    VkDescriptorSetAllocateInfo da = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=desc_pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VK_OK(vkAllocateDescriptorSets(device,&da,&desc_set));
    struct buffer out = buffer_make(WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer spill = buffer_make(GROUPS * (1536 + 1024) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct buffer args = buffer_make(12, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    const uint32_t groups[3] = {NX, NY, NZ}; memcpy(args.map,groups,sizeof groups);
    VkDescriptorBufferInfo db[2] = {{.buffer=out.buf,.range=out.size},{.buffer=spill.buf,.range=spill.size}};
    VkWriteDescriptorSet writes[2] = {
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=desc_set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db[0]},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=desc_set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db[1]}};
    vkUpdateDescriptorSets(device,2,writes,0,NULL);
    uint32_t ref[WORDS], actual[WORDS]; unsigned bad=0;
    for (int indirect=0; indirect<2; indirect++) {
        execute(argv[1],indirect,&out,&spill,&args,ref);
        for (uint32_t g=0; g<GROUPS; g++) for (uint32_t i=0; i<9; i++) {
            uint32_t expected = i == 8 ? 100 : (g*100+i) ^ (g*200+(i+1)%8) ^ (g*300+7-i) ^ 36u ^ 64u;
            if (ref[g*9+i] != expected) { if (bad++ < 5) fprintf(stderr,"reference mismatch g=%u i=%u expected=%u actual=%u\n",g,i,expected,ref[g*9+i]); }
        }
        execute(argv[2],indirect,&out,&spill,&args,actual);
        for (unsigned i=0; i<WORDS; i++) if (actual[i] != ref[i]) { if (bad++ < 5) fprintf(stderr,"spill mismatch indirect=%d word=%u expected=%u actual=%u\n",indirect,i,ref[i],actual[i]); }
    }
    printf("spill_gpu groups=%d words_per_run=%d direct_indirect=2 byte_comparisons=%zu mismatches=%u validation_errors=%u\n",GROUPS,WORDS,2*sizeof ref,bad,validation_errors);
    buffer_free(&args); buffer_free(&spill); buffer_free(&out); vkDestroyDescriptorPool(device,desc_pool,NULL);
    vkDestroyPipelineLayout(device,pipeline_layout,NULL); vkDestroyDescriptorSetLayout(device,set_layout,NULL); vkDestroyCommandPool(device,pool,NULL);
    vkDestroyDevice(device,NULL); destroy_debug(instance,debug,NULL); vkDestroyInstance(instance,NULL);
    return bad || validation_errors ? 1 : 0;
}
