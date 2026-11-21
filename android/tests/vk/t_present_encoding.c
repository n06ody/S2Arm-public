// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 原创灰阶经生产呈现路径读回交换链；期望为输入编码字节，UNORM 是负对照。
#define vkCreateImage fixture_CreateImage
#define vkDestroyImage fixture_DestroyImage
#define main unused_fixture_main
#define vkGetSwapchainImagesKHR fixture_GetSwapchainImagesKHR
#define vkQueuePresentKHR fixture_QueuePresentKHR
#include "t_backend_vk.c"
#undef main
#undef vkCreateImage
#undef vkDestroyImage
#undef vkGetSwapchainImagesKHR
#undef vkQueuePresentKHR

static VkExtent2D probe_extent;
static VkFormat probe_format;
static VkImage probe_images[64];
static unsigned probe_active, probe_value, probe_bars, probe_cases, probe_pattern, probe_flip, probe_alpha255;
static VkImage encoded_image;
static unsigned encoded_created, encoded_destroyed;

// 旁观真实驱动对象的寿命，不替换其分配或释放。
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *im) {
    VkResult r = fixture_CreateImage(d, ci, a, im);
    if (r == VK_SUCCESS && ci->extent.width == 320 && ci->extent.height == 180 &&
        (ci->format == VK_FORMAT_R8G8B8A8_SRGB || ci->format == VK_FORMAT_B8G8R8A8_SRGB)) {
        CHECK(!encoded_image, "previous encoding target must be released");
        encoded_image = *im; encoded_created++;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage im, const VkAllocationCallbacks *a) {
    if (im && im == encoded_image) { encoded_image = VK_NULL_HANDLE; encoded_destroyed++; }
    fixture_DestroyImage(d, im, a);
}
static uint64_t probe_bad, probe_pixels;
// 只从真实驱动公布的格式中选择，分别验证 RGBA / BGRA 交换链，绝不伪造能力。
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice dev, VkSurfaceKHR surface, uint32_t *n, VkSurfaceFormatKHR *out) {
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)dlsym(RTLD_NEXT,"vkGetPhysicalDeviceSurfaceFormatsKHR");
    VkSurfaceFormatKHR available[64]; uint32_t count=64;
    VkResult r=real(dev,surface,&count,available);if(r!=VK_SUCCESS)return r;
    VkFormat want=getenv("S2A_TEST_PRESENT_BGRA")?VK_FORMAT_B8G8R8A8_UNORM:VK_FORMAT_R8G8B8A8_UNORM;
    for(uint32_t i=0;i<count;i++) if(available[i].format==want && available[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        if(out && !*n)return VK_INCOMPLETE;
        if(out)*out=available[i];
        *n=1;return VK_SUCCESS;
    }
    CHECK(0,"真实表面不支持待测格式 %d",(int)want);return VK_ERROR_FORMAT_NOT_SUPPORTED;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    PFN_vkCreateSwapchainKHR real = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(dev,"vkCreateSwapchainKHR");
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_dev.physical,ci->surface,&caps)==VK_SUCCESS && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),"probe surface must support readback");
    VkSwapchainCreateInfoKHR copy=*ci; copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    probe_extent=ci->imageExtent; probe_format=ci->imageFormat;
    return real(dev,&copy,a,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR s,uint32_t *n,VkImage *im) {
    VkResult r=fixture_GetSwapchainImagesKHR(d,s,n,im);
    if(r==VK_SUCCESS && im && *n<=64) memcpy(probe_images,im,*n*sizeof *im);
    return r;
}
static void probe_readback(VkQueue q, VkImage image) {
    VkDevice d=g_dev.device;
    VkDeviceSize bytes=(VkDeviceSize)probe_extent.width*probe_extent.height*4;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; CHECK(vkCreateBuffer(d,&bi,NULL,&b)==VK_SUCCESS,"probe buffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d,b,&req);
    uint32_t mt=s2a_vk_memory_type(&g_dev,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,0);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
    VkDeviceMemory mem; CHECK(vkAllocateMemory(d,&ai,NULL,&mem)==VK_SUCCESS,"probe memory");
    CHECK(vkBindBufferMemory(d,b,mem,0)==VK_SUCCESS,"probe binding");
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=g_dev.queue_family};
    VkCommandPool pool; CHECK(vkCreateCommandPool(d,&pci,NULL,&pool)==VK_SUCCESS,"probe pool");
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(d,&cai,&cb)==VK_SUCCESS,"probe cmd");
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb,&begin)==VK_SUCCESS,"probe begin");
    VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&bar);
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={probe_extent.width,probe_extent.height,1}};
    vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b,1,&region);
    bar.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; bar.dstAccessMask=0;bar.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;bar.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&bar);
    CHECK(vkEndCommandBuffer(cb)==VK_SUCCESS,"probe end");
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence f;
    CHECK(vkCreateFence(d,&fi,NULL,&f)==VK_SUCCESS,"probe fence");
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    CHECK(vkQueueSubmit(q,1,&si,f)==VK_SUCCESS,"probe submit");
    CHECK(vkWaitForFences(d,1,&f,VK_TRUE,5000000000ull)==VK_SUCCESS,"probe wait");
    uint8_t *data=NULL; CHECK(vkMapMemory(d,mem,0,bytes,0,(void **)&data)==VK_SUCCESS,"probe map");
    uint64_t bad=0; unsigned first=0, fx=0, fy=0;
    for(uint32_t y=0;y<probe_extent.height;y++) for(uint32_t x=0;x<probe_extent.width;x++) {
        int inside=!probe_bars || (x>=40 && x<280 && y>=20 && y<160);
        unsigned want=inside?probe_value:0, alpha=inside?(probe_alpha255?255:128):255;
        const uint8_t *p=data+4*((size_t)y*probe_extent.width+x);
        unsigned rgb[3] = {want,want,want};
        if (probe_pattern) {
            // 两条水平色带，最近点放大 5 倍；CPU 期望独立于呈现实现。
            unsigned top = probe_flip ? y >= 90 : y < 90;
            rgb[0] = top ? 200 : 16; rgb[1] = top ? 64 : 128; rgb[2] = top ? 16 : 200;
            if (probe_format == VK_FORMAT_B8G8R8A8_UNORM) { unsigned tmp=rgb[0];rgb[0]=rgb[2];rgb[2]=tmp; }
        }
        int mismatch=abs((int)p[0]-(int)rgb[0])>1 || abs((int)p[1]-(int)rgb[1])>1 || abs((int)p[2]-(int)rgb[2])>1 || p[3]!=alpha;
        if(mismatch && !bad) { first=p[0]; fx=x; fy=y; }
        bad+=mismatch;probe_pixels++;
    }
    probe_cases++;probe_bad+=bad;
    printf("PRESENT_PIXELS case=%u format=%d expected=%u firstBad=%u at=%u,%u bad=%llu bars=%u\n",probe_cases,(int)probe_format,probe_value,first,fx,fy,(unsigned long long)bad,probe_bars);
    vkUnmapMemory(d,mem);vkDestroyFence(d,f,NULL);vkDestroyCommandPool(d,pool,NULL);vkDestroyBuffer(d,b,NULL);vkFreeMemory(d,mem,NULL);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q,const VkPresentInfoKHR *pi) {
    if(probe_active) probe_readback(q,probe_images[pi->pImageIndices[0]]);
    return fixture_QueuePresentKHR(q,pi);
}
static void profile_rendered_frames(void) {
    g_pass_capture_test = 1;
    CHECK(open_backend(), "pass capture open");
    nvn_gpu_err e = {0};
    nvn_gpu_surface sf = NULL;
    CHECK(OK(nvn_gpu_surface_attach((void *)1, &sf, &e)) && sf, "pass capture attach: %s", e.why);
    nvn_gpu_tex t = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    if (sf && v) {
        nvn_gpu_present_desc present = {.src = v};
        for (uint64_t i = 1; i <= 30; i++) {
            nvn_gpu_cmd c = begin_cmd();
            nvn_gpu_pass_desc p = {.w = 64, .h = 36, .layers = 1, .ncolor = 1};
            p.color[0].view = v;
            p.color[0].load = NVN_GPU_LOAD_CLEAR;
            p.color[0].store = NVN_GPU_STORE_STORE;
            nvn_gpu_pass_begin(c, &p);
            nvn_gpu_pass_end(c);
            p.color[0].load = NVN_GPU_LOAD_LOAD;
            nvn_gpu_pass_begin(c, &p);
            nvn_gpu_pass_end(c);
            CHECK(OK(nvn_gpu_cmd_commit(c, i, on_done, NULL, &e)), "pass capture commit: %s", e.why);
            nvn_gpu_drawable d = {0};
            CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &d, &e)), "pass capture acquire: %s", e.why);
            CHECK(OK(nvn_gpu_present(sf, &d, &present, i, NULL, NULL, &e)), "pass capture present: %s", e.why);
            if (i == 1) nanosleep(&(struct timespec){1, 500000000}, NULL);
        }
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "pass capture close: %s", e.why);
    unsigned want = getenv("S2A_TEST_PRESENT_BGRA") ? 0 : 24;
    CHECK(g_pass_capture_frames == want && g_pass_capture_clear == want && g_pass_capture_load == want &&
          g_pass_capture_end == (want != 0), "PASS_CAPTURE expected=%u actual=%u clear=%u load=%u end=%u",
          want, g_pass_capture_frames, g_pass_capture_clear, g_pass_capture_load, g_pass_capture_end);
    printf("PASS_CAPTURE expected=%u actual=%u clear=%u load=%u end=%u\n",
           want, g_pass_capture_frames, g_pass_capture_clear, g_pass_capture_load, g_pass_capture_end);
    CHECK(g_pass_capture_ticks == want * 2, "PASS_CAPTURE raw timestamp pairs expected=%u actual=%u",
          want * 2, g_pass_capture_ticks);
    printf("PASS_CAPTURE raw timestamp pairs expected=%u actual=%u\n", want * 2, g_pass_capture_ticks);
    g_pass_capture_test = 0;
}
static const char *const export_module_names[] = {
    "pass_capture_fixture.vert.spv", "pass_capture_fixture.frag.spv",
    "pass_draw_export_depth.frag.spv", "pass_draw_export_color.frag.spv",
};

struct export_copy_fixture {
    int depth;
    nvn_gpu_tex src, dst;
    nvn_gpu_view src_view, dst_view;
    nvn_gpu_pipe pipe;
    nvn_gpu_shader fs;
};

static nvn_gpu_pass_desc export_copy_pass(const struct export_copy_fixture *copy, nvn_gpu_view target) {
    nvn_gpu_pass_desc pass = {.w = 64, .h = 36, .layers = 1};
    const nvn_gpu_att att = {.view = target, .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
    if (copy->depth) { pass.depth = att; pass.clear_depth = 0.5f; }
    else { pass.ncolor = 1; pass.color[0] = att; }
    return pass;
}

static int export_copy_setup(struct export_copy_fixture *copy, nvn_gpu_shader vs,
                             const nvn_gpu_layout *layout, const char *dir, int depth) {
    char path[512];
    size_t bytes = 0;
    snprintf(path, sizeof path, "%s/%s", dir, export_module_names[depth ? 2 : 3]);
    void *blob = read_file(path, &bytes);
    if (!blob) return 0;
    copy->depth = depth;
    nvn_gpu_shader_src src = {.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV,
        .layout = layout, .entry = "main", .blob = blob, .bytes = bytes};
    nvn_gpu_err e = {0};
    nvn_gpu_rc rc = nvn_gpu_shader_load(&src, &copy->fs, &e);
    free(blob);
    CHECK(OK(rc), "pass draw export copy shader: %s", e.why);
    if (!OK(rc)) return 0;
    const uint32_t format = depth ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_R11G11B10F;
    copy->src = make_tex(64, 36, format, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    copy->dst = make_tex(64, 36, format, NVN_GPU_TEX_RENDER);
    if (!copy->src || !copy->dst) return 0;
    copy->src_view = make_view(copy->src);
    copy->dst_view = make_view(copy->dst);
    nvn_gpu_pipe_desc pipe = {.samples = 1, .prim = 4, .color.logic_op = 3,
        .poly.mode = NVN_POLYGON_MODE_FILL, .origin_ll = 1};
    pipe.st[NVN_GPU_SH_VS].sh = vs;
    pipe.st[NVN_GPU_SH_FS].sh = copy->fs;
    if (depth) { pipe.dfmt = format; pipe.ds.dt = pipe.ds.dw = 1; pipe.ds.df = 8; }
    else { pipe.cfmt[0] = format; pipe.chmask.mask = 0xf; }
    rc = nvn_gpu_pipe_get(&pipe, NVN_GPU_PIPE_SYNC, &copy->pipe, &e);
    CHECK(OK(rc), "pass draw export copy pipeline: %s", e.why);
    return OK(rc) && copy->src_view && copy->dst_view;
}

static void export_copy_draw(nvn_gpu_cmd cmd, const struct export_copy_fixture *copy,
                             nvn_gpu_samp samp, unsigned draws) {
    const nvn_gpu_pass_desc pass = export_copy_pass(copy, copy->dst_view);
    const nvn_gpu_binding bind = {.ent = 0, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {copy->src_view, samp}};
    nvn_gpu_pass_begin(cmd, &pass);
    nvn_gpu_set_pipe(cmd, copy->pipe);
    nvn_gpu_bind(cmd, NVN_GPU_SH_FS, &bind, 1);
    for (unsigned i = 0; i < draws; i++)
        nvn_gpu_draw(cmd, &(nvn_gpu_draw_args){.prim = 4, .count = 3, .instances = 1});
    nvn_gpu_pass_end(cmd);
}

static void inspect_pass_draw_export(const char *dir, const char *frag_sha) {
    CHECK(!strncmp(g_pass_draw_export_directory, "nvk-draw-export-", 16), "PASS_DRAW_EXPORT directory");
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/draws.tsv", dir, g_pass_draw_export_directory);
    FILE *table = fopen(path, "r");
    CHECK(table != NULL, "PASS_DRAW_EXPORT TSV exists");
    if (!table) return;
    char line[8192], sha[4][65] = {{0}}, pso[3][65] = {{0}};
    const uint32_t view_formats[3] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    };
    unsigned rows = 0;
    unsigned long long previous_seq = 0;
    CHECK(fgets(line, sizeof line, table) && strstr(line, "descriptor_sets\trun\ttextures"),
          "PASS_DRAW_EXPORT TSV header");
    while (fgets(line, sizeof line, table)) {
        unsigned long long seq;
        unsigned pass, draw, format, changed, sets, run;
        char vertex[65], fragment[65], pipeline[65], state[65], texture[4096], entry1[32], entry2[32];
        int fields = sscanf(line, "%llu\t%u\t%u\t%u\t%64s\t%31s\t%64s\t%31s\t%64s\t%64s\t%x\t%x\t%u\t%4095s",
            &seq, &pass, &draw, &format, vertex, entry1, fragment, entry2, pipeline, state, &changed, &sets, &run, texture);
        CHECK(fields == 14, "PASS_DRAW_EXPORT parse row %u fields=%d", rows, fields);
        if (fields != 14) break;
        const unsigned expected_draw = rows % 4;
        const unsigned family = expected_draw < 2 ? 0 : expected_draw - 1;
        const int unchanged = expected_draw == 1;
        char expected_texture[48];
        snprintf(expected_texture, sizeof expected_texture, "%u/0/0/0:%u:", NVN_GPU_SH_FS, view_formats[family]);
        if (!rows) strcpy(sha[0], vertex);
        if (!sha[family + 1][0]) {
            strcpy(sha[family + 1], fragment);
            strcpy(pso[family], pipeline);
        }
        CHECK(draw == expected_draw && run == (unchanged ? 2u : 1u) && format == (family ? view_formats[family] : 0) &&
              (expected_draw ? seq == previous_seq : seq > previous_seq) &&
              (unchanged ? changed == 0 && sets == 0 : changed && sets == 1) &&
              strlen(vertex) == 64 && strlen(pipeline) == 64 && strlen(state) == 64 && !strcmp(vertex, sha[0]) &&
              !strcmp(pipeline, pso[family]) && !strcmp(fragment, sha[family + 1]) &&
              !strcmp(entry1, "main") && !strcmp(entry2, "main") && strstr(texture, expected_texture) &&
              strstr(texture, ":plain:"), "PASS_DRAW_EXPORT state or binding row %u", rows);
        previous_seq = seq;
        rows++;
    }
    CHECK(!ferror(table) && rows == 96 && !strcmp(sha[1], frag_sha), "PASS_DRAW_EXPORT rows=%u", rows);
    fclose(table);
    for (unsigned i = 0; i < 4; i++) {
        size_t reference_bytes = 0, captured_bytes = 0;
        snprintf(path, sizeof path, "%s/%s", dir, export_module_names[i]);
        void *reference = read_file(path, &reference_bytes);
        snprintf(path, sizeof path, "%s/%s/%s.spv", dir, g_pass_draw_export_directory, sha[i]);
        void *captured = read_file(path, &captured_bytes);
        CHECK(reference && captured && reference_bytes == captured_bytes &&
              !memcmp(reference, captured, reference_bytes), "PASS_DRAW_EXPORT module %u bytes", i);
        free(reference);
        free(captured);
    }
}

static void profile_constant_pixels(const char *dir, const char *sha) {
    char word[9] = {0};
    memcpy(word, sha, 8);
    g_pass_key_hi = (int32_t)strtoul(word, NULL, 16);
    memcpy(word, sha + 8, 8);
    g_pass_key_lo = (int32_t)strtoul(word, NULL, 16);
    snprintf(g_pass_expected_identity, sizeof g_pass_expected_identity, "key=%.16s sha=%s", sha, sha);
    g_pass_capture_test = 2;
    int export_draws = getenv("S2A_TEST_PRESENT_BGRA") != NULL;
    if (export_draws) CHECK(!setenv("S2A_FRAME_LOG_DIR", dir, 1), "PASS_DRAW_EXPORT fixture output root");
    nvn_gpu_err e = {0};
    CHECK(open_backend(), "pass replacement open");
    nvn_gpu_surface sf = NULL;
    CHECK(OK(nvn_gpu_surface_attach((void *)1, &sf, &e)) && sf, "pass replacement attach: %s", e.why);
    if (!sf) goto done;
    char path[512];
    size_t vn = 0, fn = 0;
    snprintf(path, sizeof path, "%s/pass_capture_fixture.vert.spv", dir);
    void *vcode = read_file(path, &vn);
    snprintf(path, sizeof path, "%s/pass_capture_fixture.frag.spv", dir);
    void *fcode = read_file(path, &fn);
    CHECK(vcode && fcode, "pass replacement shader fixture files");
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_layout_ent ent = {.set = 0, .binding = 0, .count = 1, .cls = NVN_GPU_RES_TEX_SAMP,
                              .stages = 1u << NVN_GPU_SH_FS, .access = NVN_GPU_ACC_R};
    nvn_gpu_layout empty = {0}, fl = {.ent = &ent, .nent = 1};
    nvn_gpu_shader_src src = {.stage = NVN_GPU_SH_VS, .kind = NVN_GPU_BLOB_SPIRV,
                             .layout = &empty, .entry = "main", .blob = vcode, .bytes = vn};
    CHECK(OK(nvn_gpu_shader_load(&src, &vs, &e)), "pass replacement VS: %s", e.why);
    src = (nvn_gpu_shader_src){.stage = NVN_GPU_SH_FS, .kind = NVN_GPU_BLOB_SPIRV,
                              .layout = &fl, .entry = "main", .blob = fcode, .bytes = fn};
    CHECK(OK(nvn_gpu_shader_load(&src, &fs, &e)), "pass replacement FS: %s", e.why);
    free(vcode); free(fcode);
    if (!vs || !fs) goto done;
    nvn_gpu_pipe_desc pd = {.samples = 1, .prim = 4, .color.logic_op = 3, .poly.mode = NVN_POLYGON_MODE_FILL,
                            .origin_ll = 1, .chmask.mask = 0xff, .cfmt = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8UI}};
    if (export_draws) {
        pd.cfmt[2] = pd.cfmt[3] = NVN_FORMAT_RGBA8;
        pd.dfmt = NVN_FORMAT_DEPTH32F;
    }
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "pass replacement pipeline: %s", e.why);
    if (!pipe) goto done;
    nvn_gpu_tex input = make_tex(1, 1, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex shown = make_tex(64, 36, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER);
    nvn_gpu_tex aux = make_tex(64, 36, NVN_FORMAT_RGBA8UI, NVN_GPU_TEX_RENDER);
    nvn_gpu_view iv = make_view(input), sv = make_view(shown), av = make_view(aux);
    nvn_gpu_tex ext[3] = {0};
    nvn_gpu_view ext_view[3] = {0};
    if (export_draws) {
        for (unsigned i = 0; i < 3; i++) {
            ext[i] = make_tex(64, 36, i == 2 ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
            if (ext[i]) ext_view[i] = make_view(ext[i]);
            CHECK(ext_view[i], "pass draw export attachment %u", i);
            if (!ext_view[i]) goto done;
        }
    }
    samp_par sp = {0};
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "pass replacement sampler: %s", e.why);
    if (!iv || !sv || !av || !samp) goto done;
    struct export_copy_fixture copies[2] = {{0}};
    if (export_draws && (!export_copy_setup(&copies[0], vs, &fl, dir, 1) ||
                         !export_copy_setup(&copies[1], vs, &fl, dir, 0))) {
        CHECK(0, "pass draw export copy fixtures");
        goto done;
    }
    const uint8_t white[4] = {255, 255, 255, 255};
    nvn_gpu_cmd upload = begin_cmd();
    nvn_gpu_tex_upload(upload, input, 0, 0, NULL, white, 4, 4);
    if (export_draws) for (unsigned i = 0; i < 2; i++) {
        const nvn_gpu_pass_desc init = export_copy_pass(&copies[i], copies[i].src_view);
        nvn_gpu_pass_begin(upload, &init);
        nvn_gpu_pass_end(upload);
    }
    CHECK(OK(nvn_gpu_cmd_commit(upload, 1, on_done, NULL, &e)), "pass replacement upload: %s", e.why);
    nvn_gpu_present_desc present = {.src = sv};
    nvn_gpu_binding bind = {.ent = 0, .cls = NVN_GPU_RES_TEX_SAMP, .u.ts = {iv, samp}};
    int replace = !getenv("S2A_TEST_PRESENT_BGRA");
    for (uint64_t i = 2; i <= 32; i++) {
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc p = {.w = 64, .h = 36, .layers = 1, .ncolor = 2};
        p.color[0].view = sv; p.color[1].view = av;
        p.color[0].load = p.color[1].load = NVN_GPU_LOAD_CLEAR;
        p.color[0].store = p.color[1].store = NVN_GPU_STORE_STORE;
        if (export_draws) {
            p.ncolor = 4;
            p.color[2] = (nvn_gpu_att){.view = ext_view[0], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
            p.color[3] = (nvn_gpu_att){.view = ext_view[1], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
            p.depth = (nvn_gpu_att){.view = ext_view[2], .load = NVN_GPU_LOAD_CLEAR, .store = NVN_GPU_STORE_STORE};
            p.clear_depth = 1.0f;
        }
        nvn_gpu_pass_begin(c, &p);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_bind(c, NVN_GPU_SH_FS, &bind, 1);
        nvn_gpu_draw(c, &(nvn_gpu_draw_args){.prim = 4, .count = 3, .instances = 1});
        if (export_draws)
            nvn_gpu_draw(c, &(nvn_gpu_draw_args){.prim = 4, .count = 3, .instances = 1});
        nvn_gpu_pass_end(c);
        if (export_draws) {
            export_copy_draw(c, &copies[0], samp, 1);
            export_copy_draw(c, &copies[1], samp, 1);
            export_copy_draw(c, &copies[1], samp, 2); // same format, but not a single-draw pass
        }
        CHECK(OK(nvn_gpu_cmd_commit(c, i, on_done, NULL, &e)), "pass replacement commit: %s", e.why);
        nvn_gpu_drawable d = {0};
        CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &d, &e)), "pass replacement acquire: %s", e.why);
        probe_active = i >= 8 && i <= 12;
        probe_alpha255 = 1;
        probe_value = replace ? 0 : 255;
        CHECK(OK(nvn_gpu_present(sf, &d, &present, i, NULL, NULL, &e)), "pass replacement present: %s", e.why);
        probe_active = probe_alpha255 = 0;
        if (i == 2) nanosleep(&(struct timespec){1, 500000000}, NULL);
    }
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(iv); nvn_gpu_view_release(sv); nvn_gpu_view_release(av);
    nvn_gpu_tex_release(input); nvn_gpu_tex_release(shown); nvn_gpu_tex_release(aux);
    if (export_draws) {
        for (unsigned i = 0; i < 3; i++) {
            nvn_gpu_view_release(ext_view[i]);
            nvn_gpu_tex_release(ext[i]);
        }
        for (unsigned i = 0; i < 2; i++) {
            nvn_gpu_view_release(copies[i].src_view);
            nvn_gpu_view_release(copies[i].dst_view);
            nvn_gpu_tex_release(copies[i].src);
            nvn_gpu_tex_release(copies[i].dst);
            nvn_gpu_shader_release(copies[i].fs);
        }
    }
done:
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "pass replacement close: %s", e.why);
    int enabled = !getenv("S2A_TEST_PRESENT_BGRA");
    CHECK(g_pass_capture_shaders == 24 && g_pass_capture_samples == 24 &&
          (enabled ? g_pass_capture_substitutions >= 20 : g_pass_capture_substitutions == 0),
          "PASS_CONSTANT enabled=%d fs=%u bindings=%u replaced=%u",
          enabled, g_pass_capture_shaders, g_pass_capture_samples, g_pass_capture_substitutions);
    printf("PASS_CONSTANT enabled=%d fs=%u bindings=%u replaced=%u\n",
           enabled, g_pass_capture_shaders, g_pass_capture_samples, g_pass_capture_substitutions);
    CHECK(g_pass_draw_export_done == (unsigned)export_draws,
          "PASS_DRAW_EXPORT: expected one complete 24-frame export of 96 draws and 4 distinct modules when enabled");
    if (g_pass_draw_export_done) inspect_pass_draw_export(dir, sha);
    g_pass_capture_test = 0;
}
int main(int argc, char **argv) {
    char why[256]={0};if(s2a_vk_device_create(&g_dev,why,sizeof why)) {fprintf(stderr,"device %s\n",why);return 2;}
    memset(&g_desc,0,sizeof g_desc);g_desc.host_device=&g_dev;g_desc.app_label="original-present-grayscale";
    profile_rendered_frames();
    if (argc > 2 && strlen(argv[2]) == 64) profile_constant_pixels(argv[1], argv[2]);
    if(!open_backend())return 2;
    nvn_gpu_err e={0};nvn_gpu_surface sf=NULL;CHECK(OK(nvn_gpu_surface_attach((void *)1,&sf,&e)),"attach %s",e.why);
    if(!sf)return 2;
    const uint32_t formats[]={NVN_FORMAT_RGBA8_SRGB,NVN_FORMAT_RGBA8,NVN_FORMAT_BGRA8_SRGB,NVN_FORMAT_BGRA8};
    const unsigned values[]={0,16,64,128,200,255}; uint64_t ticket=1;
    for(unsigned fmt=0;fmt<4;fmt++) for(unsigned filter=0;filter<2;filter++) for(unsigned val=0;val<6;val++) {
        nvn_gpu_tex t=make_tex(64,36,formats[fmt],NVN_GPU_TEX_SAMPLED|NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_view v=make_view(t);uint8_t pixels[64*36*4];
        for(unsigned p=0;p<64*36;p++){pixels[p*4]=pixels[p*4+1]=pixels[p*4+2]=values[val];pixels[p*4+3]=128;}
        nvn_gpu_cmd c=begin_cmd();nvn_gpu_tex_upload(c,t,0,0,NULL,pixels,64*4,sizeof pixels);
        CHECK(OK(nvn_gpu_cmd_commit(c,ticket,on_done,NULL,&e)),"upload %s",e.why);
        nvn_gpu_drawable dr={0};CHECK(OK(nvn_gpu_drawable_acquire(sf,1000000000ull,NULL,&dr,&e)),"acquire %s",e.why);
        nvn_gpu_present_desc pd={0};pd.src=v;pd.filter=filter;pd.flip_y=filter;
        probe_bars=filter; if(probe_bars){pd.crop[0]=4;pd.crop[1]=2;pd.crop[2]=48;pd.crop[3]=24;pd.dst[0]=40;pd.dst[1]=20;pd.dst[2]=240;pd.dst[3]=140;}
        probe_value=values[val];probe_active=1;
        CHECK(OK(nvn_gpu_present(sf,&dr,&pd,ticket,on_shown,NULL,&e)),"present %s",e.why);probe_active=0;
        CHECK(OK(nvn_gpu_fence_wait(ticket++,5000000000ull,&e)),"wait %s",e.why);
        nvn_gpu_view_release(v);nvn_gpu_tex_release(t);
    }
    // 灰阶本身无法检验通道顺序与 Y 翻转；用不对称彩色条带逐像素核对。
    for (unsigned fmt=0;fmt<4;fmt++) for (unsigned flip=0;flip<2;flip++) {
        nvn_gpu_tex t=make_tex(64,36,formats[fmt],NVN_GPU_TEX_SAMPLED|NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_view v=make_view(t); uint8_t pixels[64*36*4];
        for (unsigned y=0;y<36;y++) for (unsigned x=0;x<64;x++) {
            uint8_t *p=pixels+4*(y*64+x); unsigned top=y<18;
            p[0]=top?200:16;p[1]=top?64:128;p[2]=top?16:200;p[3]=128;
            if (fmt>=2) { uint8_t tmp=p[0];p[0]=p[2];p[2]=tmp; }
        }
        nvn_gpu_cmd c=begin_cmd();nvn_gpu_tex_upload(c,t,0,0,NULL,pixels,64*4,sizeof pixels);
        CHECK(OK(nvn_gpu_cmd_commit(c,ticket,on_done,NULL,&e)),"pattern upload");
        nvn_gpu_drawable dr={0};CHECK(OK(nvn_gpu_drawable_acquire(sf,1000000000ull,NULL,&dr,&e)),"pattern acquire");
        nvn_gpu_present_desc pd={0};pd.src=v;pd.flip_y=flip;
        probe_pattern=probe_active=1;probe_flip=flip;probe_bars=0;
        CHECK(OK(nvn_gpu_present(sf,&dr,&pd,ticket,on_shown,NULL,&e)),"pattern present");probe_active=probe_pattern=0;
        CHECK(OK(nvn_gpu_fence_wait(ticket++,5000000000ull,&e)),"pattern wait");
        nvn_gpu_view_release(v);nvn_gpu_tex_release(t);
    }
    // EASU 上屏放大（开关打开）：sRGB 源 + 非线性 UNORM 交换链，非整数倍缩放必须走片元 EASU。
    // 平坦灰阶要穿过「硬件解码 → EASU → 写编码字节」整条链回到源字节（±1）；矩形外仍是黑边。
    // alpha 由 EASU 固定写 1.0（交换链是不透明合成），这是与 blit 唯一的有意差别。
    {
        const unsigned val = 64;
        g_upscale_flag = 1;
        nvn_gpu_tex t = make_tex(64, 36, NVN_FORMAT_RGBA8_SRGB, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
        nvn_gpu_view v = make_view(t); uint8_t pixels[64*36*4];
        for (unsigned p = 0; p < 64*36; p++) { pixels[p*4]=pixels[p*4+1]=pixels[p*4+2]=(uint8_t)val; pixels[p*4+3]=128; }
        nvn_gpu_cmd c = begin_cmd(); nvn_gpu_tex_upload(c, t, 0, 0, NULL, pixels, 64*4, sizeof pixels);
        CHECK(OK(nvn_gpu_cmd_commit(c, ticket, on_done, NULL, &e)), "upscale upload %s", e.why);
        nvn_gpu_drawable dr = {0};
        CHECK(OK(nvn_gpu_drawable_acquire(sf, 1000000000ull, NULL, &dr, &e)), "upscale acquire %s", e.why);
        nvn_gpu_present_desc pd = {0}; pd.src = v; pd.filter = 1;
        pd.crop[0]=4; pd.crop[1]=2; pd.crop[2]=48; pd.crop[3]=24;   // 48x24 → 240x140：3.75 / 5.83 倍，非整数
        pd.dst[0]=40; pd.dst[1]=20; pd.dst[2]=240; pd.dst[3]=140;
        probe_bars=1; probe_value=val; probe_alpha255=1; probe_pattern=0; probe_active=1;
        uint64_t hits0 = s2a_vk_easu_hits();
        CHECK(OK(nvn_gpu_present(sf, &dr, &pd, ticket, on_shown, NULL, &e)), "upscale present %s", e.why);
        probe_active=0;
        uint64_t dh = s2a_vk_easu_hits() - hits0;
        CHECK(dh == 1, "upscale: EASU 应被选中，实际 %llu（回落 blit 说明适用条件不满足）", (unsigned long long)dh);
        printf("PRESENT_UPSCALE SRGB→UNORM：EASU 次数 %llu\n", (unsigned long long)dh);
        CHECK(OK(nvn_gpu_fence_wait(ticket++, 5000000000ull, &e)), "upscale wait %s", e.why);
        g_upscale_flag = -1; probe_bars=0; probe_alpha255=0;
        nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    }
    CHECK(encoded_created==1 && encoded_destroyed==0,"encoding target reused: %u/%u",encoded_created,encoded_destroyed);
    CHECK(OK(nvn_gpu_surface_rebuild(sf,&e)),"rebuild encoding target %s",e.why);
    CHECK(!encoded_image && encoded_destroyed==1,"rebuild releases encoding target once");
    nvn_gpu_tex t=make_tex(64,36,NVN_FORMAT_RGBA8_SRGB,NVN_GPU_TEX_SAMPLED|NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v=make_view(t); uint8_t px[64*36*4];memset(px,128,sizeof px);
    nvn_gpu_cmd c=begin_cmd();nvn_gpu_tex_upload(c,t,0,0,NULL,px,64*4,sizeof px);
    CHECK(OK(nvn_gpu_cmd_commit(c,ticket,on_done,NULL,&e)),"post rebuild upload");
    nvn_gpu_drawable dr={0};CHECK(OK(nvn_gpu_drawable_acquire(sf,1000000000ull,NULL,&dr,&e)),"post rebuild acquire");
    nvn_gpu_present_desc pd={0};pd.src=v;probe_bars=0;probe_value=128;probe_active=1;
    CHECK(OK(nvn_gpu_present(sf,&dr,&pd,ticket,on_shown,NULL,&e)),"post rebuild present");probe_active=0;
    nvn_gpu_view_release(v);nvn_gpu_tex_release(t);
    // 不 detach：close 持 g_mu，仍须收掉带中间图的面，不能递归拿锁。
    CHECK(OK(nvn_gpu_close(5000000000ull,&e)),"close grayscale session");
    CHECK(!encoded_image && encoded_created==2 && encoded_destroyed==2,"close releases recreated encoding target once: %u/%u",encoded_created,encoded_destroyed);
    CHECK(open_backend(),"reopen lifecycle session");
    c71_present();
    CHECK(OK(nvn_gpu_close(5000000000ull,&e)),"close %s",e.why);s2a_vk_device_destroy(&g_dev,1);
    printf("PRESENT_RESULT cases=%u pixels=%llu bad=%llu API_failures=%d\n",probe_cases,(unsigned long long)probe_pixels,(unsigned long long)probe_bad,g_fail);
    return g_fail||probe_bad?1:0;
}
