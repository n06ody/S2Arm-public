// Draw the program selected by the production LibPack resolver and read all MRT values.
struct mrt_pixel { const void *value; uint32_t bytes; };

static int render_mrt(const uint32_t fmts[4], const struct rp_prog *program,
                      const struct mrt_pixel expected[3], uint64_t signal) {
    nvn_gpu_pipe_desc pd = {.samples=1, .prim=4};
    memcpy(pd.cfmt, fmts, 4 * sizeof *fmts);
    pd.st[0].sh=program->sh[0]; pd.st[1].sh=program->sh[1];
    pd.chmask.mask=0xffff; pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.color.logic_op=3;
    nvn_gpu_err err={0}; nvn_gpu_pipe pipe=NULL;
    if (!OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err))) return 1;
    nvn_gpu_tex tex[4]; nvn_gpu_view views[4];
    nvn_gpu_pass_desc pass={.ncolor=4,.w=4,.h=4,.layers=1};
    for (int i=0;i<4;i++) {
        tex[i]=make_tex(4,4,fmts[i],NVN_GPU_TEX_RENDER);
        views[i]=make_view(tex[i]);
        pass.color[i].view=views[i]; pass.color[i].load=NVN_GPU_LOAD_CLEAR; pass.color[i].store=NVN_GPU_STORE_STORE;
    }
    uint8_t initial[3 * 256]; memset(initial,0xee,sizeof initial);
    nvn_gpu_buf pixels=make_buf(sizeof initial,NVN_GPU_MEM_SHARED,initial);
    nvn_gpu_cmd cmd=begin_cmd();
    nvn_gpu_pass_begin(cmd,&pass); nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_dyn dyn={.nvp=1,.has_sc=1};
    dyn.vp[0][2]=dyn.vp[0][3]=4; dyn.vp[0][5]=1; dyn.sc[2]=dyn.sc[3]=4;
    nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_draw_args args={.prim=4,.count=3,.instances=1}; nvn_gpu_draw(cmd,&args);
    nvn_gpu_pass_end(cmd);
    for (int i=0;i<3;i++)
        nvn_gpu_copy_tex_buf(cmd,tex[i+1],0,0,NULL,pixels,i*256,4*expected[i].bytes,16*expected[i].bytes);
    if (!OK(nvn_gpu_cmd_commit(cmd,signal,on_done,NULL,&err)) || !OK(nvn_gpu_fence_wait(signal,5000000000ull,&err))) return 1;
    for (int slot=0;slot<3;slot++) for (int pixel=0;pixel<16;pixel++)
        CHECK(!memcmp(buf_cpu(pixels)+slot*256+pixel*expected[slot].bytes,expected[slot].value,expected[slot].bytes),
              "MRT slot %d pixel %d",slot+1,pixel);
    nvn_gpu_buf_release(pixels,NULL,NULL);
    for (int i=0;i<4;i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(tex[i]); }
    return g_fail != 0;
}

static const uint32_t mrt_uint_formats[] = {NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RG16UI};
static const uint8_t mrt_uint_values[3][4] = {{13,37,69,255},{21,58,95,121},{1,1,1,2}};
static const struct mrt_pixel mrt_uint_pixels[] = {
    {mrt_uint_values[0],4}, {mrt_uint_values[1],4}, {mrt_uint_values[2],4},
};

static int test_mrt(int compatible) {
    struct rp_shader_req q = {.prim=4, .rt_count=4};
    memcpy(q.rt_format, mrt_uint_formats, sizeof mrt_uint_formats);
    const struct rp_prog *program = NULL;
    int rc = rp_shader_resolve(&q, &program);
    CHECK(!rc && program && program->lib_sourced == compatible && compile_calls == !compatible,
          "MRT admission: compatible=%d rc=%d compiler=%d", compatible, rc, compile_calls);
    if (g_fail || render_mrt(mrt_uint_formats, program, mrt_uint_pixels, 1)) return 1;
    rp_shader_reset();
    nvn_gpu_err err={0};
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)),"close MRT session"); s2a_vk_device_destroy(&g_dev,1);
    printf("libpack MRT compatible=%d compiler=%d integer_pixels=48 failures=%d\n",compatible,compile_calls,g_fail);
    return g_fail!=0;
}

static int test_mrt_variants(void) {
    const uint32_t float_formats[] = {NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA32F, NVN_FORMAT_RGBA32F, NVN_FORMAT_RG32F};
    const uint32_t sint_formats[] = {NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8I, NVN_FORMAT_RGBA8I, NVN_FORMAT_RG16I};
    const uint32_t mixed_formats[] = {NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RG16I};
    const float float_values[][4] = {{13,37,69,255}, {21,58,95,121}, {257,513}};
    const int8_t sint_values[][4] = {{-13,37,-69,99}, {21,58,95,121}};
    const int16_t sint_rg[] = {257,513}, mixed_rg[] = {-257,513};
    const struct mrt_pixel float_pixels[] = {{float_values[0],16}, {float_values[1],16}, {float_values[2],8}};
    const struct mrt_pixel sint_pixels[] = {{sint_values[0],4}, {sint_values[1],4}, {sint_rg,4}};
    const struct mrt_pixel mixed_pixels[] = {{mrt_uint_values[0],4}, {mrt_uint_values[1],4}, {mixed_rg,4}};
    const uint32_t *formats[] = {float_formats, mrt_uint_formats, sint_formats, mixed_formats};
    const struct mrt_pixel *pixels[] = {float_pixels, mrt_uint_pixels, sint_pixels, mixed_pixels};
    const unsigned order[] = {0,1,2,3,1,0};
    const uint64_t hits = s2a_libpack_hits();
    struct nvk_prog_rec warm = {.stage_mask=3, .lp_ok=2};
    memcpy(warm.keys, guest_keys, 2 * sizeof warm.keys[0]);
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++) {
        unsigned variant = order[i];
        struct rp_shader_req q = {.prim=4, .rt_count=4};
        memcpy(q.rt_format, formats[variant], sizeof float_formats);
        rp_shader_reset();   // Keep the LibPack memo, discard the program table.
        const struct rp_prog *program = NULL;
        int rc = rp_shader_resolve(&q, &program);
        CHECK(!rc && program && program->lib_sourced && !compile_calls,
              "MRT variants: attachment set %u hits LibPack without compiling (rc=%d compiles=%d)", variant, rc, compile_calls);
        CHECK(s2a_libpack_hits() == hits + (i < 4 ? i + 1 : 4),
              "MRT variants: each numeric mask has its own memo entry (arm=%u)", i);
        if (g_fail || render_mrt(formats[variant], program, pixels[variant], i + 1)) return 1;
        if (i == 1) memcpy(warm.identity, program->identity, sizeof warm.identity);
    }
    rp_shader_reset();
    CHECK(nvk_prog_warm_build(&warm) == 0, "MRT variants: old warm record builds the default Float module");
    struct rp_shader_req q = {.prim=4, .rt_count=4};
    memcpy(q.rt_format, mrt_uint_formats, sizeof mrt_uint_formats);
    const struct rp_prog *program = NULL;
    int rc = rp_shader_resolve(&q, &program);
    CHECK(!rc && program && program->lib_sourced && !compile_calls, "MRT variants: old warm entry retries with actual Uint RTs");
    if (g_fail || render_mrt(mrt_uint_formats, program, mrt_uint_pixels, 7)) return 1;
    rp_shader_reset();
    nvn_gpu_err err={0};
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)), "close MRT variant session");
    s2a_vk_device_destroy(&g_dev,1);
    printf("libpack MRT variants: draws=7 pixels=336 compiler=%d failures=%d\n", compile_calls, g_fail);
    return g_fail != 0;
}
